#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <immintrin.h>

// Typed definitions
typedef char i8;
typedef unsigned char u8;
typedef unsigned short u16;
typedef int i32;
typedef unsigned u32;
typedef unsigned long u64;

// Thread-safe synchronization and shared state
typedef struct {
    u8* pixel_data;      // Pointer to image data
    u32 width;           // Image width
    u32 row_size;        // Bytes per row
    u32 height;          // Image height
    u32 found_header;    // Shared header detection
    u16 message_len;     // Detected message length
    int thread_id;       // Current thread ID
    int total_threads;   // Total number of threads
    pthread_mutex_t mutex; // Mutex for thread-safe operations
} ThreadProcessArgs;

// Macro for error handling
#define PRINT_ERROR(cstring) write(STDERR_FILENO, cstring, sizeof(cstring) - 1)

// Packed BMP header structure
#pragma pack(1)
struct bmp_header {
    i8  signature[2];
    u32 file_size;
    u32 unused_0;
    u32 data_offset;
    u32 info_header_size;
    u32 width;
    u32 height;
    u16 number_of_planes;
    u16 bit_per_pixel;
    u32 compression_type;
    u32 compressed_image_size;
};

// SIMD-optimized message detection function
void* parallel_message_detect(void* args) {
    ThreadProcessArgs* thread_args = (ThreadProcessArgs*)args;

    // Calculate rows for this thread
    u32 start_row = thread_args->thread_id *
                    (thread_args->height / thread_args->total_threads);
    u32 end_row = (thread_args->thread_id + 1) *
                  (thread_args->height / thread_args->total_threads);

    // Adjust last thread to cover remaining rows
    if (thread_args->thread_id == thread_args->total_threads - 1) {
        end_row = thread_args->height;
    }

    // Use AVX2 for faster comparisons
    __m256i target_blue  = _mm256_set1_epi8(127);
    __m256i target_green = _mm256_set1_epi8(188);
    __m256i target_red   = _mm256_set1_epi8(217);

    for (u32 y = start_row; y < end_row; y++) {
        for (u32 x = 0; x < thread_args->width; x++) {
            u32 pixel_index = (y * thread_args->row_size) + (x * 4);

            // Aligned SIMD comparison
            __m256i pixel_vec = _mm256_loadu_si256(
                (__m256i*)&thread_args->pixel_data[pixel_index]
            );

            // Extract color channels for comparison
            __m256i blue_mask  = _mm256_cmpeq_epi8(
                _mm256_and_si256(pixel_vec, _mm256_set1_epi8(0xFF)),
                target_blue
            );
            __m256i green_mask = _mm256_cmpeq_epi8(
                _mm256_and_si256(_mm256_srli_epi16(pixel_vec, 8), _mm256_set1_epi8(0xFF)),
                target_green
            );
            __m256i red_mask   = _mm256_cmpeq_epi8(
                _mm256_and_si256(_mm256_srli_epi16(pixel_vec, 16), _mm256_set1_epi8(0xFF)),
                target_red
            );

            // Combine comparison results
            __m256i combined_mask = _mm256_and_si256(
                _mm256_and_si256(blue_mask, green_mask),
                red_mask
            );

            // If match found, attempt to synchronize and process
            if (_mm256_movemask_epi8(combined_mask)) {
                pthread_mutex_lock(&thread_args->mutex);
                // Existing header detection logic
                if (thread_args->found_header == 0) {
                    thread_args->found_header = x;
                } else if (x > thread_args->found_header) {
                    // Potential message detection logic
                    thread_args->message_len =
                        thread_args->pixel_data[pixel_index] +
                        thread_args->pixel_data[pixel_index + 2];
                    pthread_mutex_unlock(&thread_args->mutex);
                    return NULL;
                }
                pthread_mutex_unlock(&thread_args->mutex);
            }
        }
    }

    return NULL;
}

int main(int argc, char** argv) {
    // Input validation
    if (argc != 2) {
        PRINT_ERROR("Usage: decode <input_filename>\n");
        return 1;
    }

    // File mapping
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        PRINT_ERROR("Failed to open file\n");
        return 1;
    }

    struct stat file_stat;
    fstat(fd, &file_stat);

    void* mapped_file = mmap(NULL, file_stat.st_size,
                              PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (mapped_file == MAP_FAILED) {
        PRINT_ERROR("Memory mapping failed\n");
        return 1;
    }

    // BMP Header parsing
    struct bmp_header* header = (struct bmp_header*)mapped_file;
    u8* pixel_data = (u8*)(mapped_file + header->data_offset);
    u32 row_size = ((header->width * header->bit_per_pixel + 31) / 32) * 4;

    // Determine optimal thread count
    int num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    num_threads = (num_threads > 8) ? 8 : num_threads; // Cap at 8

    // Thread and synchronization setup
    pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
    ThreadProcessArgs* thread_args = malloc(num_threads * sizeof(ThreadProcessArgs));

    // Initialize shared processing arguments
    for (int i = 0; i < num_threads; i++) {
        thread_args[i].pixel_data = pixel_data;
        thread_args[i].width = header->width;
        thread_args[i].height = header->height;
        thread_args[i].row_size = row_size;
        thread_args[i].thread_id = i;
        thread_args[i].total_threads = num_threads;
        thread_args[i].found_header = 0;
        thread_args[i].message_len = 0;
        pthread_mutex_init(&thread_args[i].mutex, NULL);
    }

    // Create threads for parallel processing
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL,
                       parallel_message_detect, &thread_args[i]);
    }

    // Wait for threads to complete
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // Cleanup
    free(threads);
    free(thread_args);
    munmap(mapped_file, file_stat.st_size);

    return 0;
}
