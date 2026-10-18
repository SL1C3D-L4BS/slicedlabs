// sl-compile-only
// P2900 contracts: pre, post and contract_assert. Compile-only: gcc 16 links a violation handler
// the probe does not supply.
int sl_checked(int x) pre(x > 0) post(r : r > 0) {
  contract_assert(x != 100);
  return x;
}

int main() { return 0; }
