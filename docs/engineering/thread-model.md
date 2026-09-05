# Thread Model

## Main / IPC thread

The process main thread owns the IPC server run loop and handles accepted requests serially in the current implementation.

## WPA event monitor

`WpaEventMonitor` owns a worker `std::thread`, an atomic running flag, a mutex and a snapshot. The worker updates WPA event state; readers obtain a synchronized snapshot.

## Boundary rule

Service and IPC code may request operations through platform/backend functions. Mechanism execution (`wpa_cli`, `udhcpc`, `ifconfig`, route/DNS mutation) belongs in the platform/backend layer, not in product UI/client code.

## Follow-up verification

The brownfield adoption still requires explicit teardown/race tests and target restart/reconnect evidence before production promotion.
