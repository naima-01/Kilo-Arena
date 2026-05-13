# Kilo-Arena: Custom Memory Allocator for Text Editor

A modified version of the Kilo text editor that replaces `malloc/free` with a custom arena allocator, demonstrating lower allocation overhead and simplified memory management.

---

## Project Goals

This project explores OS memory management concepts through:

- Custom arena allocation
- Allocation tracking
- Memory alignment
- Buffer management
- Performance analysis
- Integration into a real text editor

---

## Features

- Custom arena allocator implementation
- Allocation statistics tracking
- Text editor integration
- Syntax highlighting
- File editing and saving
- Search functionality
- Real-time arena usage display

---


## Building

```bash
# Clone the repository
git clone <your-repo-url>
cd kilo

# Compile the editor
gcc -o kilo_arena kilo_arena.c arena.c

# Or use the provided Makefile
make
```

---

## Usage

```bash
# Create/Open a file
./kilo_arena <filename>.txt
```

### Editor Commands

- `Ctrl+S` — Save file
- `Ctrl+Q` — Quit editor
- `Ctrl+F` — Find text
- `Ctrl+D` — Show arena statistics
- Arrow keys — Navigate
- Type — Insert text

---

## Testing

### Correctness Test

```bash
gcc -o test_correctness test_arena_correctness.c arena.c
./test_correctness
```

### Performance Benchmark

```bash
gcc -o test_performance test_arena_performance.c arena.c
./test_performance
```

---

## Requirements

- Linux / WSL / macOS
- GCC Compiler
- Standard C Libraries

