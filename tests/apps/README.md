# Test fixture apps

Each folder here is a complete Fourfold app folder (`code/`, and where needed `data/`, `private/`, …) used by tests, such as the compiler's golden tests. Tests find this folder via the `FOURFOLD_TEST_APPS_DIR` macro.

Keep fixtures small and focused on what they test. Sample apps meant for people to run and read belong in `examples/` instead.
