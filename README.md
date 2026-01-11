# Log Based Key Value File System in C

This project implements a basic key value file system that performs raw and direct I O operations on a block device. It focuses on performance by combining in memory buffering, asynchronous disk writes, read caching, and multi threading.

The system provides a simple abstraction for appending and reading data while hiding low level device interactions from the user.



## Overview

- Implements a log structured file system design
- Uses in memory write buffering with periodic flushes to disk
- Supports asynchronous background writes using a worker thread
- Includes read caching to improve read performance
- Ensures thread safe access to shared data structures



## Project Structure

### Files

- logfs.c  
  Implements the core file system logic including buffering, caching, synchronization, and disk I O.

- device.h  
  Defines the interface for interacting with a block device. Includes operations such as open, close, read, write, get size, and get block size.

- system.h  
  Provides system level function declarations and utilities used by the file system.



## Design and Behavior

- Write operations are first stored in an in memory write buffer.
- A background worker thread periodically flushes buffered data to disk.
- Read operations first consult the read cache before accessing the device.
- This design reduces disk I O frequency and improves throughput.
- Flushing ensures data consistency between memory and disk.



## Key Functions

### logfs_open(const char *pathname)
- Opens the underlying block device
- Initializes in memory buffers, caches, and worker thread
- Returns a handle to the file system instance

### logfs_close(struct logfs *file_system)
- Flushes any remaining buffered data
- Stops background threads
- Releases allocated resources

### logfs_read(struct logfs *file_system, void *buffer, uint64_t offset, size_t length)
- Reads data starting at the given offset
- Uses the read cache when possible
- Falls back to direct device reads if needed

### logfs_append(struct logfs *file_system, const void *buffer, uint64_t length)
- Appends data to the in memory write buffer
- Does not immediately write to disk
- Improves performance by batching writes

### flush_to_disk(struct logfs *file_system)
- Writes buffered data to the block device
- Ensures durability and consistency
- Can be triggered manually or by the worker thread



## Threading Model

- A dedicated worker thread handles asynchronous disk writes
- Mutexes and synchronization primitives ensure thread safe access
- Readers and writers can safely operate concurrently


