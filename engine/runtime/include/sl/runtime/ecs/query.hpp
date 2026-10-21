#pragma once

// ECS queries (WO-090, SL-ARCH-003 ARCH3-R114 to R117, ADR-0025, ADR-0026; episode 093). A query
// declares its terms as type tags: read<T> (const T&), write<T> (T&), optional<T> (T*, nullptr
// when absent) or optional<const T> (const T*), and the filters with<T>, without<T>, changed<T>
// and added<T>, which pass no argument. make() checks the terms (query_invalid for contradictory
// or duplicate terms and above 32 terms) and derives the access set: reads are read,
// optional<const T>, changed and added; writes are write and optional<T>; with and without touch
// no column. The match cache holds the creation indices of the matched archetypes and the
// archetype generation it last saw; a new generation scans only the archetypes created since
// (ARCH3-R115), a world restored or swapped rescans everything. Rows are visited in (archetype
// creation, chunk, row) order; changed<T>(since) and added<T>(since) keep the rows whose tick is
// later than iter_args::since under the wrap rule (std::int32_t(tick - since) > 0), all filters
// together; a write term stamps changed[row] = iter_args::write_tick on every visited row
// (ARCH3-R117). Iteration holds world::begin_iteration(), so a structural call inside a body trips
// the Debug iteration guard (ARCH3-R112); structural requests go through command buffers.
// par_for_each cuts the matched chunks, in creation order, into batches of whole chunks that
// close once they hold at least par_batch_min_rows rows (a partition of the data only), runs one
// frame job per batch with the batch index as the ordered_key chunk_index, and waits (helping)
// before it returns (ARCH3-R116). Bodies must be noexcept. Sources: src/ecs_query.cpp (terms and
// access set), src/ecs_query_match.cpp (match cache). Layer L6: includes ecs/world.hpp and
// jobs.hpp.
// Thread safety (`thread_safety`): for_each, for_each_chunk and par_for_each are worker-only
// (inside a system with matching declared access; the owner thread outside phases). A query
// object belongs to one system: its cache and counters change in every call, so two threads must
// not iterate the same query object at once. par_for_each bodies run concurrently on workers and
// must not share mutable state beyond their own rows.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/sync.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace sl::runtime::ecs {

// --- limits (SL-ARCH-003 `limits`) ---

/// The most terms of one query (an archetype has at most 32 components).
inline constexpr std::uint32_t max_query_terms = max_components_per_archetype;
/// A par_for_each batch closes once it holds at least this many rows (`limits`
/// par_batch_min_rows, whole chunks).
inline constexpr std::uint32_t par_batch_min_rows = 4096;

/// What a term does. optional_read and optional_write are optional<const T> and optional<T>.
enum class term_kind : std::uint8_t {
  read,
  write,
  with,
  without,
  optional_read,
  optional_write,
  changed,
  added
};

namespace detail {

/// A component type a term may name: a registered component type, not const, not a reference.
template <class T>
concept term_component =
    component_type<T> && !std::is_const_v<T> && !std::is_volatile_v<T> && !std::is_reference_v<T>;

} // namespace detail

/// Reads T: the body receives const T&; T is required.
template <class T> struct read {
  static_assert(detail::term_component<T>, "read<T> names a plain component type");
  using component = T;
  static constexpr term_kind kind = term_kind::read;
};
/// Writes T: the body receives T&; T is required and every visited row's changed tick is stamped.
template <class T> struct write {
  static_assert(detail::term_component<T>, "write<T> names a plain component type");
  using component = T;
  static constexpr term_kind kind = term_kind::write;
};
/// Requires T; no argument.
template <class T> struct with {
  static_assert(detail::term_component<T>, "with<T> names a plain component type");
  using component = T;
  static constexpr term_kind kind = term_kind::with;
};
/// Excludes T; no argument.
template <class T> struct without {
  static_assert(detail::term_component<T>, "without<T> names a plain component type");
  using component = T;
  static constexpr term_kind kind = term_kind::without;
};
/// T when present: optional<T> passes T* (a write, stamped where present), optional<const T>
/// passes const T* (a read); nullptr when the archetype lacks T.
template <class T> struct optional {
  static_assert(detail::term_component<std::remove_const_t<T>>,
                "optional<T> names a component type, optionally const");
  using component = std::remove_const_t<T>;
  static constexpr term_kind kind =
      std::is_const_v<T> ? term_kind::optional_read : term_kind::optional_write;
};
/// Requires T and keeps the rows whose changed tick of T is later than iter_args::since.
template <class T> struct changed {
  static_assert(detail::term_component<T>, "changed<T> names a plain component type");
  using component = T;
  static constexpr term_kind kind = term_kind::changed;
};
/// Requires T and keeps the rows whose added tick of T is later than iter_args::since.
template <class T> struct added {
  static_assert(detail::term_component<T>, "added<T> names a plain component type");
  using component = T;
  static constexpr term_kind kind = term_kind::added;
};

/// The components a query reads and writes, each ascending and unique; a component in writes is
/// not repeated in reads.
struct component_access {
  std::vector<component_id> reads;
  std::vector<component_id> writes;

  friend bool operator==(const component_access &, const component_access &) = default;
};

/// The ticks of one iteration: changed and added filters keep rows with a tick later than since;
/// write terms stamp write_tick.
struct iter_args {
  std::uint32_t since = 0;
  std::uint32_t write_tick = 0;
};

/// True when `tick` is later than `since` under the wrap rule of WO-093 (a signed 32-bit
/// difference above zero).
[[nodiscard]] constexpr bool tick_later(std::uint32_t tick, std::uint32_t since) noexcept {
  return static_cast<std::int32_t>(tick - since) > 0;
}

namespace detail {

/// One term as make() resolved it.
struct query_term {
  term_kind kind = term_kind::with;
  component_id id;
};

/// True for the terms that pass an argument to the body.
[[nodiscard]] constexpr bool passes_argument(term_kind kind) noexcept {
  return kind == term_kind::read || kind == term_kind::write || kind == term_kind::optional_read ||
         kind == term_kind::optional_write;
}

/// The non-template part of a query: the checked terms, the access set and the match cache.
class query_core {
public:
  /// Checks `terms` (query_invalid: above 32 terms, a term given twice, with and without of one
  /// component, without with any other term of the same component, two argument terms of one
  /// component such as read and write) and derives the access set and the match sets.
  [[nodiscard]] result<void> init(const component_registry &registry,
                                  std::span<const query_term> terms) noexcept;
  /// Brings the match cache up to date with `w`: only the archetypes created since the last
  /// refresh are scanned, unless `w` is another world or was restored (then all of them).
  void refresh(const world &w) noexcept;
  /// True when `a` holds every required component and no excluded one.
  [[nodiscard]] bool matches(const archetype &a) const noexcept;
  /// True when `w` uses the registry the query was made with.
  [[nodiscard]] bool same_registry(const world &w) const noexcept {
    return &w.registry() == registry_;
  }
  /// The creation indices of the matched archetypes, ascending.
  [[nodiscard]] std::span<const std::uint32_t> matched() const noexcept { return matched_; }
  [[nodiscard]] const component_access &access() const noexcept { return access_; }
  /// Archetypes the match cache examined, since make.
  [[nodiscard]] std::uint64_t archetypes_visited() const noexcept { return archetypes_visited_; }
  /// Non-empty chunks iterated, since make.
  [[nodiscard]] std::uint64_t chunks_visited() const noexcept { return chunks_visited_; }
  void count_chunks(std::uint64_t n) noexcept { chunks_visited_ += n; }

private:
  const component_registry *registry_ = nullptr;
  std::array<component_id, max_query_terms> required_{}; // ascending, unique
  std::array<component_id, max_query_terms> excluded_{}; // ascending, unique
  std::uint32_t required_count_ = 0;
  std::uint32_t excluded_count_ = 0;
  component_access access_;
  std::vector<std::uint32_t> matched_; // room for max_archetypes, reserved by init
  const world *seen_ = nullptr;        // compared, never dereferenced
  std::uint64_t generation_ = 0;
  std::uint32_t scanned_ = 0;
  std::uint64_t archetypes_visited_ = 0;
  std::uint64_t chunks_visited_ = 0;
};

/// The argument of term `Term` for `row` of `column` (nullptr for an absent optional component).
template <class Term>
[[nodiscard]] decltype(auto) term_ref(void *column, std::uint32_t row) noexcept {
  using C = typename Term::component;
  if constexpr (Term::kind == term_kind::read) {
    return static_cast<const C *>(column)[row];
  } else if constexpr (Term::kind == term_kind::write) {
    return static_cast<C *>(column)[row];
  } else if constexpr (Term::kind == term_kind::optional_read) {
    return column != nullptr ? static_cast<const C *>(column) + row
                             : static_cast<const C *>(nullptr);
  } else {
    static_assert(Term::kind == term_kind::optional_write, "term_ref of a filter term");
    return column != nullptr ? static_cast<C *>(column) + row : static_cast<C *>(nullptr);
  }
}

/// The rows [first, first + count) of `column` as a span (empty for an absent optional component).
template <class Term>
[[nodiscard]] auto term_span(void *column, std::uint32_t first, std::uint32_t count) noexcept {
  using C = typename Term::component;
  if constexpr (Term::kind == term_kind::read || Term::kind == term_kind::optional_read) {
    if (column == nullptr) {
      return std::span<const C>{};
    }
    return std::span<const C>{static_cast<const C *>(column) + first, count};
  } else {
    static_assert(Term::kind == term_kind::write || Term::kind == term_kind::optional_write,
                  "term_span of a filter term");
    if (column == nullptr) {
      return std::span<C>{};
    }
    return std::span<C>{static_cast<C *>(column) + first, count};
  }
}

} // namespace detail

/// A query over the terms `Terms...` (term tags). Movable; made with make().
template <class... Terms> class query {
  static constexpr std::size_t term_count = sizeof...(Terms);
  using term_list = std::tuple<Terms...>;
  template <std::size_t I> using term_at = std::tuple_element_t<I, term_list>;
  static constexpr std::array<term_kind, term_count> kinds{Terms::kind...};
  static constexpr std::size_t argument_count =
      (std::size_t{0} + ... + (detail::passes_argument(Terms::kind) ? 1U : 0U));

  [[nodiscard]] static consteval std::array<std::size_t, argument_count> argument_positions() {
    std::array<std::size_t, argument_count> out{};
    std::size_t j = 0;
    for (std::size_t i = 0; i < term_count; ++i) {
      if (detail::passes_argument(kinds[i])) {
        out[j] = i;
        ++j;
      }
    }
    return out;
  }
  static constexpr std::array<std::size_t, argument_count> arg_pos = argument_positions();
  using arg_seq = std::make_index_sequence<argument_count>;

  template <std::size_t J>
  using arg_t = decltype(detail::term_ref<term_at<arg_pos[J]>>(nullptr, 0));
  template <std::size_t J>
  using span_t = decltype(detail::term_span<term_at<arg_pos[J]>>(nullptr, 0, 0));

  template <class F, std::size_t... J>
  [[nodiscard]] static consteval bool row_body(std::index_sequence<J...> /*seq*/) {
    return std::is_nothrow_invocable_v<F &, entity, arg_t<J>...>;
  }
  template <class F, std::size_t... J>
  [[nodiscard]] static consteval bool chunk_body(std::index_sequence<J...> /*seq*/) {
    return std::is_nothrow_invocable_v<F &, std::span<const entity>, span_t<J>...>;
  }
  template <class F, std::size_t... J>
  [[nodiscard]] static consteval bool par_body(std::index_sequence<J...> /*seq*/) {
    return std::is_nothrow_invocable_v<F &, std::uint32_t, entity, arg_t<J>...>;
  }

  /// The columns and ticks of one chunk, per term: columns for argument terms, the changed ticks
  /// of write terms (stamped) and the changed or added ticks of filter terms (tested).
  struct chunk_view {
    std::array<void *, term_count> columns{};
    std::array<std::uint32_t *, term_count> ticks{};
    const entity *entities = nullptr;
    std::uint32_t rows = 0;
  };

  /// One par_for_each batch: the chunks from (first_slot, first_chunk) to (last_slot,
  /// last_chunk), both included; slots index the matched archetypes.
  struct batch_range {
    std::uint32_t first_slot = 0;
    std::uint32_t first_chunk = 0;
    std::uint32_t last_slot = 0;
    std::uint32_t last_chunk = 0;
    std::uint32_t index = 0;
  };

  /// What every batch job of one par_for_each call shares (it lives on the caller's stack, which
  /// waits for every job).
  template <class Fn> struct par_context {
    const query *self = nullptr;
    const world *target = nullptr;
    iter_args args;
    Fn *fn = nullptr;
    std::atomic<std::uint32_t> ran{0};
  };

  /// The job body of one batch (32 bytes, inline in job_fn).
  template <class Fn> struct batch_job {
    par_context<Fn> *ctx = nullptr;
    batch_range range;

    void operator()(jobs::job_context & /*context*/) noexcept {
      ctx->self->run_batch(*ctx, range);
      ctx->ran.fetch_add(1);
    }
  };

public:
  /// A query over `registry` (the world's registry): component_unknown for an unregistered type,
  /// query_invalid for contradictory or duplicate terms or above 32 terms.
  [[nodiscard]] static result<query> make(const component_registry &registry) noexcept {
    if (term_count > max_query_terms) {
      return make_error(errc::query_invalid, "query: more than 32 terms");
    }
    const std::array<result<component_id>, term_count> found{
        registry.id_of<typename Terms::component>()...};
    query q;
    std::array<detail::query_term, term_count> terms{};
    for (std::size_t i = 0; i < term_count; ++i) {
      if (!found[i]) {
        return std::unexpected(found[i].error());
      }
      q.ids_[i] = *found[i];
      terms[i] = detail::query_term{.kind = kinds[i], .id = *found[i]};
    }
    auto checked = q.core_.init(registry, terms);
    if (!checked) {
      return std::unexpected(checked.error());
    }
    return q;
  }

  /// The components the terms read and write.
  [[nodiscard]] const component_access &access() const noexcept { return core_.access(); }

  /// Calls fn(entity, args...) for every selected row in (archetype creation, chunk, row) order;
  /// args are const T& for read, T& for write, T* or const T* for optional. Nothing is visited
  /// when `w` uses another registry.
  template <class F> void for_each(world &w, const iter_args &args, F &&fn) noexcept {
    auto &&body = std::forward<F>(fn);
    using Fn = std::remove_reference_t<decltype(body)>;
    static_assert(row_body<Fn>(arg_seq{}), "for_each needs a noexcept fn(entity, args...)");
    if (!core_.same_registry(w)) {
      return;
    }
    const iteration_guard guard = w.begin_iteration();
    core_.refresh(w);
    const std::span<const archetype *const> all = w.archetypes();
    for (const std::uint32_t index : core_.matched()) {
      const archetype &a = *all[index];
      for (std::uint32_t chunk = 0; chunk < a.chunk_count(); ++chunk) {
        const chunk_view c = load(a, chunk);
        if (c.rows == 0) {
          break; // rows are dense: every later chunk is empty too
        }
        core_.count_chunks(1);
        for (std::uint32_t row = 0; row < c.rows; ++row) {
          if (selected(c, row, args.since)) {
            call_row(body, c.entities[row], c, row, arg_seq{});
            stamp(c, row, 1, args.write_tick);
          }
        }
      }
    }
  }

  /// Calls fn(entities, spans...) for every run of consecutive selected rows of a chunk (the
  /// whole chunk without change filters), in iteration order; spans are std::span<const T> for
  /// read and optional<const T>, std::span<T> for write and optional<T> (empty when absent).
  template <class F> void for_each_chunk(world &w, const iter_args &args, F &&fn) noexcept {
    auto &&body = std::forward<F>(fn);
    using Fn = std::remove_reference_t<decltype(body)>;
    static_assert(chunk_body<Fn>(arg_seq{}),
                  "for_each_chunk needs a noexcept fn(std::span<const entity>, spans...)");
    if (!core_.same_registry(w)) {
      return;
    }
    const iteration_guard guard = w.begin_iteration();
    core_.refresh(w);
    const std::span<const archetype *const> all = w.archetypes();
    for (const std::uint32_t index : core_.matched()) {
      const archetype &a = *all[index];
      for (std::uint32_t chunk = 0; chunk < a.chunk_count(); ++chunk) {
        const chunk_view c = load(a, chunk);
        if (c.rows == 0) {
          break;
        }
        core_.count_chunks(1);
        std::uint32_t row = 0;
        while (row < c.rows) {
          if (!selected(c, row, args.since)) {
            ++row;
            continue;
          }
          const std::uint32_t first = row;
          while (row < c.rows && selected(c, row, args.since)) {
            ++row;
          }
          call_run(body, c, first, row - first, arg_seq{});
          stamp(c, first, row - first, args.write_tick);
        }
      }
    }
  }

  /// Calls fn(chunk_index, entity, args...) for every selected row, one frame job per batch of
  /// whole chunks (a batch closes at par_batch_min_rows rows or more); chunk_index is the batch
  /// index (the ordered_key chunk_index), rows keep their order inside a batch. Waits, helping,
  /// until every batch ran. invalid_argument when `w` uses another registry; a submit error
  /// other than job_capacity (which runs the batch on the caller) stops submitting and is
  /// returned after the submitted batches finished; job_failed when a batch did not run.
  template <class F>
  [[nodiscard]] result<void> par_for_each(world &w, jobs::job_system &js, const iter_args &args,
                                          F &&fn) noexcept {
    auto &&body = std::forward<F>(fn);
    using Fn = std::remove_reference_t<decltype(body)>;
    static_assert(par_body<Fn>(arg_seq{}),
                  "par_for_each needs a noexcept fn(std::uint32_t, entity, args...)");
    if (!core_.same_registry(w)) {
      return make_error(errc::invalid_argument, "query: the world uses another registry");
    }
    const iteration_guard guard = w.begin_iteration();
    core_.refresh(w);
    par_context<Fn> ctx;
    ctx.self = this;
    ctx.target = &w;
    ctx.args = args;
    ctx.fn = &body;
    job_counter pending;
    result<void> outcome;
    std::uint32_t batches = 0;
    std::uint32_t rows_in = 0;
    bool open = false;
    batch_range range;
    const std::span<const archetype *const> all = w.archetypes();
    const std::span<const std::uint32_t> matched = core_.matched();
    for (std::uint32_t slot = 0; slot < matched.size() && outcome; ++slot) {
      const archetype &a = *all[matched[slot]];
      for (std::uint32_t chunk = 0; chunk < a.chunk_count() && outcome; ++chunk) {
        const std::uint32_t rows = a.rows(chunk);
        if (rows == 0) {
          break;
        }
        core_.count_chunks(1);
        if (!open) {
          range = batch_range{.first_slot = slot, .first_chunk = chunk, .index = batches};
          open = true;
        }
        range.last_slot = slot;
        range.last_chunk = chunk;
        rows_in += rows;
        if (rows_in >= par_batch_min_rows) {
          outcome = launch(js, ctx, range, pending);
          ++batches;
          rows_in = 0;
          open = false;
        }
      }
    }
    if (open && outcome) {
      outcome = launch(js, ctx, range, pending);
      ++batches;
    }
    const result<void> waited = js.wait(pending);
    if (!outcome) {
      return outcome;
    }
    if (!waited) {
      return waited;
    }
    if (ctx.ran.load() != batches) {
      return make_error(errc::job_failed, "query: a par_for_each batch did not run");
    }
    return {};
  }

  /// Archetypes the match cache examined since make (a refresh with no new archetype adds 0).
  [[nodiscard]] std::uint64_t archetypes_visited() const noexcept {
    return core_.archetypes_visited();
  }
  /// Non-empty chunks iterated since make (par_for_each counts every chunk of its batches).
  [[nodiscard]] std::uint64_t chunks_visited() const noexcept { return core_.chunks_visited(); }

private:
  query() noexcept = default;

  [[nodiscard]] chunk_view load(const archetype &a, std::uint32_t chunk) const noexcept {
    chunk_view c;
    c.rows = a.rows(chunk);
    c.entities = a.entities(chunk).data();
    for (std::size_t i = 0; i < term_count; ++i) {
      const component_id id = ids_[i];
      switch (kinds[i]) {
      case term_kind::read:
      case term_kind::optional_read:
        c.columns[i] = a.column(id, chunk);
        break;
      case term_kind::write:
      case term_kind::optional_write:
        c.columns[i] = a.column(id, chunk);
        c.ticks[i] = a.changed_ticks(id, chunk).data();
        break;
      case term_kind::changed:
        c.ticks[i] = a.changed_ticks(id, chunk).data();
        break;
      case term_kind::added:
        c.ticks[i] = a.added_ticks(id, chunk).data();
        break;
      case term_kind::with:
      case term_kind::without:
        break;
      }
    }
    return c;
  }

  /// True when every changed and added filter keeps `row`.
  [[nodiscard]] static bool selected(const chunk_view &c, std::uint32_t row,
                                     std::uint32_t since) noexcept {
    for (std::size_t i = 0; i < term_count; ++i) {
      if ((kinds[i] == term_kind::changed || kinds[i] == term_kind::added) &&
          !tick_later(c.ticks[i][row], since)) {
        return false;
      }
    }
    return true;
  }

  /// Stamps the changed tick of every write term present on rows [first, first + count).
  static void stamp(const chunk_view &c, std::uint32_t first, std::uint32_t count,
                    std::uint32_t tick) noexcept {
    for (std::size_t i = 0; i < term_count; ++i) {
      if ((kinds[i] == term_kind::write || kinds[i] == term_kind::optional_write) &&
          c.ticks[i] != nullptr) {
        for (std::uint32_t r = first; r < first + count; ++r) {
          c.ticks[i][r] = tick;
        }
      }
    }
  }

  template <class Fn, std::size_t... J>
  static void call_row(Fn &fn, entity e, [[maybe_unused]] const chunk_view &c,
                       [[maybe_unused]] std::uint32_t row,
                       std::index_sequence<J...> /*seq*/) noexcept {
    fn(e, detail::term_ref<term_at<arg_pos[J]>>(c.columns[arg_pos[J]], row)...);
  }

  template <class Fn, std::size_t... J>
  static void call_par_row(Fn &fn, std::uint32_t batch, entity e,
                           [[maybe_unused]] const chunk_view &c, [[maybe_unused]] std::uint32_t row,
                           std::index_sequence<J...> /*seq*/) noexcept {
    fn(batch, e, detail::term_ref<term_at<arg_pos[J]>>(c.columns[arg_pos[J]], row)...);
  }

  template <class Fn, std::size_t... J>
  static void call_run(Fn &fn, const chunk_view &c, std::uint32_t first, std::uint32_t count,
                       std::index_sequence<J...> /*seq*/) noexcept {
    fn(std::span<const entity>{c.entities + first, count},
       detail::term_span<term_at<arg_pos[J]>>(c.columns[arg_pos[J]], first, count)...);
  }

  /// Visits the chunks of `range` (worker side; reads the cache, writes only its own rows).
  template <class Fn>
  void run_batch(const par_context<Fn> &ctx, const batch_range &range) const noexcept {
    const std::span<const archetype *const> all = ctx.target->archetypes();
    const std::span<const std::uint32_t> matched = core_.matched();
    for (std::uint32_t slot = range.first_slot; slot <= range.last_slot; ++slot) {
      const archetype &a = *all[matched[slot]];
      const std::uint32_t begin = slot == range.first_slot ? range.first_chunk : 0;
      const std::uint32_t end = slot == range.last_slot ? range.last_chunk + 1 : a.chunk_count();
      for (std::uint32_t chunk = begin; chunk < end; ++chunk) {
        const chunk_view c = load(a, chunk);
        for (std::uint32_t row = 0; row < c.rows; ++row) {
          if (selected(c, row, ctx.args.since)) {
            call_par_row(*ctx.fn, range.index, c.entities[row], c, row, arg_seq{});
            stamp(c, row, 1, ctx.args.write_tick);
          }
        }
      }
    }
  }

  /// Submits one batch job (job_capacity runs the batch here instead).
  template <class Fn>
  [[nodiscard]] result<void> launch(jobs::job_system &js, par_context<Fn> &ctx,
                                    const batch_range &range, job_counter &pending) const noexcept {
    auto submitted = js.try_submit(jobs::job_desc{.fn = batch_job<Fn>{.ctx = &ctx, .range = range},
                                                  .prio = jobs::priority::frame,
                                                  .aff = jobs::affinity::any,
                                                  .name = "ecs.par_for_each"},
                                   {}, &pending);
    if (submitted) {
      return {};
    }
    if (submitted.error().code == errc::job_capacity) {
      run_batch(ctx, range);
      ctx.ran.fetch_add(1);
      return {};
    }
    return std::unexpected(submitted.error());
  }

  detail::query_core core_;
  std::array<component_id, term_count> ids_{};
};

} // namespace sl::runtime::ecs
