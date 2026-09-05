# C++ Engineering Profile

- Standard: C++17.
- Target class: Embedded Linux application / process service.
- Exceptions: currently toolchain default; no module-level no-exception contract has been frozen yet.
- RTTI: currently toolchain default.
- Concurrency: pthread/std::thread; explicit thread model documented separately.
- Allocation: standard C++ dynamic allocation is currently permitted.
- Warning policy: product CI introduces `-Wall -Wextra -Wpedantic -Werror` for the host build.
- Runtime safety: product CI adds ASan + UBSan build/smoke coverage; production target sanitizer support is not yet claimed.
- Static analysis: EEP P2/P8 adoption requires governed static analysis in a follow-up product CI integration; absence remains production evidence debt until archived evidence is produced.
