/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * logfs.c
 */

#include <pthread.h>
#include "device.h"
#include "logfs.h"
#include <string.h>

#define WRITE_CACHE_BLOCK_COUNT 32
#define READ_CACHE_BLOCK_COUNT 256

struct logfs {
    struct device *device_handle;
    char* write_buffer;
    char* write_buffer_unaligned;
    int buffer_head_index;
    int buffer_tail_index;
    pthread_mutex_t synch_mutex;
    pthread_cond_t space_available_con;
    pthread_cond_t item_available_con;
    pthread_cond_t flush_con;
    int block_size;
    int write_buffer_total;
    int write_buffer_curr;
    int should_exit_worker_thread;
    int curr_file_offset;
    pthread_t worker_thread_id;
    struct ReadCacheBlock *read_cache_blocks[READ_CACHE_BLOCK_COUNT];
};

struct ReadCacheBlock {
    int is_valid;
    int block_identifier;
    char* block_data;
    char* block_data_unaligned;
};

void* write_to_disk(void* args) {
    struct logfs *file_system = (struct logfs*)args;
    char* error_message = (char*)malloc(sizeof(char));
    int current_block_id;
    int read_cache_index;
    pthread_mutex_lock(&file_system->synch_mutex);
    while(!file_system->should_exit_worker_thread) {
        while(file_system->write_buffer_curr < file_system->block_size) {
            if(file_system->should_exit_worker_thread) {
                pthread_mutex_unlock(&file_system->synch_mutex);
                FREE(error_message);
                pthread_exit(NULL);
            }
            pthread_cond_wait(&file_system->item_available_con, &file_system->synch_mutex);
        }
        current_block_id = file_system->curr_file_offset / file_system->block_size;
        read_cache_index = current_block_id % READ_CACHE_BLOCK_COUNT;
        if(file_system->read_cache_blocks[read_cache_index]->block_identifier == current_block_id) {
            file_system->read_cache_blocks[read_cache_index]->is_valid = 0;
        }
        if(device_write(file_system->device_handle, 
                        file_system->write_buffer + file_system->buffer_tail_index, 
                        file_system->curr_file_offset, 
                        file_system->block_size)) {
            perror(error_message);
            printf("%d\n", errno);
            TRACE("Error in device_write\n");
            pthread_exit(NULL);
        }
        file_system->buffer_tail_index = (file_system->buffer_tail_index + file_system->block_size) % file_system->write_buffer_total;
        file_system->write_buffer_curr -= file_system->block_size;
        file_system->curr_file_offset += file_system->block_size;
        if(file_system->buffer_head_index == file_system->buffer_tail_index || 
           (file_system->buffer_tail_index == 0 && file_system->buffer_head_index == file_system->write_buffer_total)) {
            pthread_cond_signal(&file_system->flush_con);
        }
    }
    FREE(error_message);
    pthread_exit(NULL);
}

struct logfs *logfs_open(const char *pathname) {
    struct logfs *file_system;
    int index;
    if(!(file_system = (struct logfs*)malloc(sizeof(struct logfs)))) {
        TRACE("Error in logfs malloc");
        return NULL;
    }
    if(!(file_system->device_handle = device_open(pathname))) {
        TRACE("Error in device open");
        return NULL;
    }
    file_system->block_size = device_block(file_system->device_handle);
    file_system->write_buffer_total = WRITE_CACHE_BLOCK_COUNT * file_system->block_size;
    file_system->write_buffer_unaligned = (char*)malloc(file_system->write_buffer_total + file_system->block_size);
    file_system->write_buffer = (char*) memory_align(file_system->write_buffer_unaligned, file_system->block_size);
    memset(file_system->write_buffer, 0, file_system->write_buffer_total);
    file_system->buffer_head_index = 0;
    file_system->buffer_tail_index = 0;
    file_system->write_buffer_curr = 0;
    file_system->should_exit_worker_thread = 0;
    file_system->curr_file_offset = 0;
    for (index = 0; index < READ_CACHE_BLOCK_COUNT; ++index) {
        file_system->read_cache_blocks[index] = (struct ReadCacheBlock *)malloc(sizeof(struct ReadCacheBlock));
        file_system->read_cache_blocks[index]->block_data_unaligned = (char*)malloc(2 * file_system->block_size);
        file_system->read_cache_blocks[index]->block_data = (char*) memory_align(file_system->read_cache_blocks[index]->block_data_unaligned, file_system->block_size);
        file_system->read_cache_blocks[index]->block_identifier = -1;
        file_system->read_cache_blocks[index]->is_valid = 0;
    }
    if(pthread_mutex_init(&file_system->synch_mutex, NULL) ||
       pthread_cond_init(&file_system->space_available_con, NULL) ||
       pthread_cond_init(&file_system->item_available_con, NULL) ||
       pthread_cond_init(&file_system->flush_con, NULL)) {
        TRACE("Error in mutex and cond init");
        return NULL;
    }
    if(pthread_create(&file_system->worker_thread_id, NULL, &write_to_disk, file_system)) {
        TRACE("Error in pthread_create");
        return NULL;
    }
    return file_system;
}

int flush_to_disk(struct logfs *file_system) {
    int distance_to_move_head;
    pthread_mutex_lock(&file_system->synch_mutex);
    distance_to_move_head = file_system->block_size - (file_system->buffer_head_index % file_system->block_size);
    file_system->buffer_head_index += distance_to_move_head;
    file_system->write_buffer_curr += distance_to_move_head;
    pthread_cond_signal(&file_system->item_available_con);
    pthread_cond_wait(&file_system->flush_con, &file_system->synch_mutex);
    file_system->buffer_head_index -= distance_to_move_head;
    file_system->buffer_tail_index = file_system->buffer_tail_index == 0 
        ? file_system->buffer_head_index - (file_system->buffer_head_index % file_system->block_size) 
        : file_system->buffer_tail_index - file_system->block_size;
    file_system->write_buffer_curr = file_system->buffer_head_index % file_system->block_size;
    file_system->curr_file_offset -= file_system->block_size;
    pthread_mutex_unlock(&file_system->synch_mutex);
    return 0;
}

void logfs_close(struct logfs *file_system) {
    int index; 
    flush_to_disk(file_system);
    pthread_mutex_lock(&file_system->synch_mutex);
    file_system->should_exit_worker_thread = 1;
    pthread_mutex_unlock(&file_system->synch_mutex);
    pthread_cond_signal(&file_system->item_available_con);
    pthread_join(file_system->worker_thread_id, NULL);
    for (index = 0; index < READ_CACHE_BLOCK_COUNT; ++index) {
        FREE(file_system->read_cache_blocks[index]->block_data_unaligned);
        FREE(file_system->read_cache_blocks[index]);
    }
    FREE(file_system->write_buffer_unaligned);
    device_close(file_system->device_handle);
    FREE(file_system);
}

int logfs_read(struct logfs *file_system, void *buffer, uint64_t offset, size_t length) {
    int block_id;
    int read_cache_index;
    int read_start_offset;
    int length_to_read;
    size_t read_so_far = 0;
    flush_to_disk(file_system);
    block_id = offset / file_system->block_size;
    read_cache_index = block_id % READ_CACHE_BLOCK_COUNT;
    read_start_offset = offset % file_system->block_size;
    length_to_read = MIN(length, (size_t)file_system->block_size - read_start_offset);
    
    while(read_so_far < length) {
        if(file_system->read_cache_blocks[read_cache_index] != NULL && 
           file_system->read_cache_blocks[read_cache_index]->is_valid && 
           file_system->read_cache_blocks[read_cache_index]->block_identifier == block_id) {
            memcpy((char*)buffer + read_so_far, 
                   file_system->read_cache_blocks[read_cache_index]->block_data + read_start_offset, 
                   length_to_read);   
        }
        else {
            if((device_read(file_system->device_handle, 
                            file_system->read_cache_blocks[read_cache_index]->block_data, 
                            block_id * file_system->block_size, 
                            file_system->block_size))) {
                TRACE("Error while calling device_read");
                return -1;
            }
            file_system->read_cache_blocks[read_cache_index]->is_valid = 1;
            file_system->read_cache_blocks[read_cache_index]->block_identifier = block_id;
            memcpy((char*)buffer + read_so_far, 
                   file_system->read_cache_blocks[read_cache_index]->block_data + read_start_offset, 
                   length_to_read);
        }
        read_so_far += length_to_read;
        block_id++;
        read_cache_index = block_id % READ_CACHE_BLOCK_COUNT;
        read_start_offset = 0;
        length_to_read = MIN((size_t)file_system->block_size, length - read_so_far);
    }
    return 0;
}

int logfs_append(struct logfs *file_system, const void *buffer, uint64_t length) {
    uint64_t remaining_length = length;
    pthread_mutex_lock(&file_system->synch_mutex);
    while(remaining_length > 0) {
        while(file_system->write_buffer_total <= file_system->write_buffer_curr) {
            pthread_cond_wait(&file_system->space_available_con, &file_system->synch_mutex);
        }
        file_system->write_buffer_curr++;
        memcpy(file_system->write_buffer + file_system->buffer_head_index, 
               (char*)buffer + (length - remaining_length), 1);
        remaining_length--;
        file_system->buffer_head_index = (file_system->buffer_head_index + 1) % (file_system->write_buffer_total);
        pthread_cond_signal(&file_system->item_available_con);
    }
    pthread_mutex_unlock(&file_system->synch_mutex);
    return 0;
}