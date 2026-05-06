# Elevator OS Scheduler

**CS 4352 – Operating Systems | Final Project**
**Author:** Aarti Krishan Khatri (R11860380)
**Date:** May 2026

---

## Overview

This project implements a multithreaded elevator scheduler in C that communicates with an Elevator OS simulation via HTTP API. The scheduler reads a building configuration file, then assigns incoming passengers to the most appropriate elevator using a least-load algorithm with round-robin tiebreaking.

Three concurrent POSIX threads run in a producer-consumer pipeline:

```
[Input Thread] --> [Person Queue] --> [Scheduler Thread] --> [Assignment Queue] --> [Output Thread]
```

- **Input Thread** – polls `/NextInput` for arriving passengers and pushes them to the person queue
- **Scheduler Thread** – pops passengers, picks the best elevator, pushes assignments to the assignment queue
- **Output Thread** – pops assignments and calls `/AddPersonToElevator` on the simulation API

---

## Files

| File | Description |
|------|-------------|
| `main.c` | Full source code |
| `makefile` | Build configuration |

---

## How to Compile

Requires GCC with pthreads support. Works on Linux, HPCC, or WSL.

```bash
make
```

To remove the compiled binary:

```bash
make clean
```

---

## How to Run

```bash
./scheduler_os <building_file> <port_number>
```

**Example:**

```bash
./scheduler_os building.txt 8080
```

The building file defines each elevator on its own line in this format:

```
<name> <lowest_floor> <highest_floor> <starting_floor> <capacity>
```

**Example `building.txt`:**

```
ElevatorA 1 10 1 10
ElevatorB 5 20 5 8
```

---

## Scheduling Algorithm

1. Filter elevators that cover both the passenger's start and end floor
2. Among valid elevators, select the one with the fewest current assignments
3. Break ties with round-robin to distribute load evenly

---

## Dependencies

- GCC (C11)
- POSIX threads (`pthreads`)
- POSIX sockets
- Elevator OS simulation running on `localhost` at the given port

---

## Notes

- The simulation must be running before launching the scheduler
- Tested on Texas Tech HPCC and WSL (Ubuntu)
- Max 64 elevators supported per building file
