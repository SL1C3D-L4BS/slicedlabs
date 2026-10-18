int guarded(int x) {
  try { // PLANT
    return x;
  } catch (...) {
    return 0;
  }
}
