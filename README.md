# Roadside Message Sign Manager

Systems programming project developed for the **Operating Systems 2** course in the Informatics Engineering BSc at **ISEC (Instituto Superior de Engenharia de Coimbra)**.

### Overview
A multi-process distributed platform developed in C/C++ for Windows using native Win32 APIs, designed to control and monitor a network of roadside variable-message signs in real time. The solution splits operational responsibilities across three decoupled processes coordinated via advanced Inter-Process Communication (IPC) and synchronization primitives.

### System Architecture
The solution consists of three specialized components:
- **Central Controller (`central.exe`):** Core management daemon responsible for command processing, tracking active road alerts, coordinating display durations, and publishing fleet state.
- **Sign Node (`placar.exe`):** Virtual sign process simulating a roadside hardware panel, parsing alert messages, counting countdown intervals, and reporting local state transitions.
- **Dashboard Monitor (`monitor.exe`):** Native Win32 desktop application providing paginated real-time visualization of sign states via an event-driven graphical interface.

### Win32 IPC & Concurrency Design
- **Central-Sign Channel (Named Pipes):**
  - Bidirectional communication implemented with Win32 Named Pipes and asynchronous Overlapped I/O, maintaining non-blocking messaging pipelines across connected sign nodes.
- **Central-Monitor Pipeline (Shared Memory & Synchronization):**
  - Inter-process data sharing using file mappings (`CreateFileMapping`, `MapViewOfFile`) hosting the global alert array structure (`SHM_ALERTA`).
  - Race-condition mitigation using a named Win32 `Mutex` (`CreateMutex`, `OpenMutex`) ensuring atomic read/write access across multiple concurrent GUI consumers.
  - Low-latency event dispatching driven by a named manual-reset `Event` object, unblocking background monitor threads via `WaitForMultipleObjects` upon state mutations or shutdown commands.
- **Native GUI & GDI Rendering:**
  - Standard message-pump architecture (`GetMessage`, `TranslateMessage`, `DispatchMessage`) registered via `RegisterClassEx` and `CreateWindow`.
  - Client-area rendering on `WM_PAINT` using memory device contexts (double-buffering) and `BitBlt` to prevent display flickering.
  - Dynamic paging controls (Page Up / Page Down), modal Win32 dialog boxes for runtime resource mapping, and an asynchronous reconnection polling loop resilient to out-of-order process startups.
