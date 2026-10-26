# SQLite

SQL database engine. Version 3.50.4, the amalgamation from
https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip: `sqlite3.c` and
`sqlite3.h` only (not the shell). Public domain.

Built with the options in `../CMakeLists.txt`: no extension loading, no
deprecated APIs, and multi-thread mode (each connection used by one thread at
a time). Fourfold reaches it only through `platform`'s `Sqlite` wrapper.
