/* Host process plugin */

#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <cstdio>

#include "config.h"
#include "symcat.h"
#include "libgomp-plugin.h"
#include "oacc-plugin.h"
#include "gomp-constants.h"
#include "oacc-int.h"

#define MAX_DEVICES 1
#define MAX_ALLOCATIONS 100
#define HOST_PROCESS_DEBUG(...) DEBUG_LOG ("HOST_PROCESS debug: ", __VA_ARGS__)

const char *
GOMP_OFFLOAD_get_name (void)
{
    return "host_process";
}

unsigned int
GOMP_OFFLOAD_get_caps (void)
{
    return GOMP_OFFLOAD_CAP_NATIVE_EXEC;
}

int
GOMP_OFFLOAD_get_type (void)
{
    return OFFLOAD_TARGET_TYPE_HOST;
}

int
GOMP_OFFLOAD_get_num_devices (unsigned int omp_requires_mask)
{
    return 1;
}

// struct to track allocations
typedef struct {
    void* ptr;
    size_t size;
} allocation;


// struct to store device information
// I thought pthread_mutex_t can be important if multiple threads
// will try to initialize, modify, or interact with the simulated device concurrently.
typedef struct {
    bool initialized;
    pthread_mutex_t lock;
    pid_t process_id;
    allocation allocations[MAX_ALLOCATIONS];
} simulated_device;


// Assuming i have an array of devices
simulated_device devices[MAX_DEVICES];

bool
GOMP_OFFLOAD_init_device(int n)
{
    if (n < 0 || n >= MAX_DEVICES) {
        GOMP_PLUGIN_error ("Request to initialize non-existent simulated device %i\n", n);
        return false;
    }

    simulated_device *device = &devices[n];

    if (device->initialized) {
        return true; // Already initialized
    }

    // Initialize a mutex for this device
    if (pthread_mutex_init(&device->lock, NULL) != 0) {
        GOMP_PLUGIN_error("Failed to initialize a mutex for simulated device %d\n", n);
        return false;
    }

    // Fork a new process to simulate the device
    pid_t pid = fork();
    if (pid == -1) {
        GOMP_PLUGIN_error("Failed to fork a simulated device process");
        return false;
    } else if (pid == 0) {
        // Here we are in the child process and we need to setup the environment for simulation, initialize communication channels, etc.
//        while (true) {
//            pause();
//        }
//        _exit(0);
    }

    // Parent process
    device->process_id = pid;
    device->initialized = true;
    HOST_PROCESS_DEBUG("Simulated device %d initialized with process ID %d\n", n, pid);

    return true;
}


bool
GOMP_OFFLOAD_fini_device(int n)
{
    if (n < 0 || n >= MAX_DEVICES) {
        GOMP_PLUGIN_error ("Request to operate on non-existent device %i", n);
        return false;
    }

    simulated_device *device = &devices[n];

    if (!device->initialized) {
        return true;
    }

    // Terminate the simulated device process
    if (device->process_id != 0) {
        if (kill(device->process_id, SIGTERM) == -1) {
            GOMP_PLUGIN_error("Failed to terminate simulated device process");
            return false;
        }

        // Wait for the process to terminate
        int status;
        if (waitpid(device->process_id, &status, 0) == -1) {
            GOMP_PLUGIN_error("Failed to wait for simulated device process to terminate");
            return false;
        }
    }

    // destroy the mutex
    if (pthread_mutex_destroy(&device->lock) != 0) {
        GOMP_PLUGIN_error("Failed to destroy mutex for simulated device %d\n", n);
        return false;
    }

    // Current device is marked uninitialized
    device->initialized = false;
    device->process_id = 0;

    return true;
}



unsigned
GOMP_OFFLOAD_version (void)
{
    return GOMP_VERSION;
}

typedef struct {
    void *code;         // Pointer to the compiled code
    size_t code_size;   // Size of the code
    void **data;        // Pointers to any relevant data
    size_t data_size;   // Size of the data
} host_process_image_desc;

// Do i understand correctly that "Reverse offloading" refers to the scenario where
// functions that are normally executed on the device are instead executed on the host ?
// Will i need to handle indirect or reverse function calls ?
int
GOMP_OFFLOAD_load_image(int n, unsigned version, const void *target_data,
                            struct addr_pair **target_table,
                            uint64_t **rev_fn_table,
                            uint64_t *host_ind_fn_table)
{
    host_process_image_desc *img = (host_process_image_desc *)target_data;

    // Here we create a unique name for the shared memory object with the device number
    char shm_name[256];
    snprintf(shm_name, sizeof(shm_name), "/my_shm_%d", n);

    // Open shared memory object, where
    // "/my_shm_n" -- name of the shared memory object for device № n,
    // "O_CREAT | O_RDWR" -- shared memory object should be created if it does not already exist and it should be opened for reading and writing
    // "0666" -- file can be read and written by the user, group, and others
    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        GOMP_PLUGIN_error("shm_open");
        return -1;
    }

    // Here we calculate the total size needed in shared memory
    // and resize the shared memory object pointed to by the file descriptor fd to a specified length
    size_t total_size = img->code_size + img->data_size;
    if (ftruncate(fd, total_size) == -1) {
        GOMP_PLUGIN_error("ftruncate");
        close(fd);
        shm_unlink(shm_name);
        return -1;
    }

    // Here I map the shared memory object represented by the file descriptor fd into the process's virtual address space
    void *shm_addr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_addr == MAP_FAILED) {
        GOMP_PLUGIN_error("mmap");
        close(fd);
        shm_unlink(shm_name);
        return -1;
    }

    // Copy the code and data into the shared memory
    memcpy(shm_addr, img->code, img->code_size);
    memcpy((char *)shm_addr + img->code_size, img->data, img->data_size);

    // Allocate the target_table with 2 entries: one for code, one for data
    *target_table = malloc(2 * sizeof(addr_pair));
    if (*target_table == NULL) {
        GOMP_PLUGIN_error("malloc failed");
        munmap(shm_addr, total_size);
        close(fd);
        shm_unlink(shm_name);
        return -1;
    }

    // Set up the address pairs for code and data
    (*target_table)[0].start = (uintptr_t) shm_addr;
    (*target_table)[0].end = (uintptr_t) shm_addr + img->code_size;
    (*target_table)[1].start = (uintptr_t) (shm_addr + img->code_size);
    (*target_table)[1].end = (uintptr_t) (shm_addr + img->code_size + img->data_size);

    // Clean up the file descriptor
    close(fd);

    return 0;
}

bool
GOMP_OFFLOAD_unload_image(int n, unsigned version, const void *target_data)
{
//    if (GOMP_VERSION_DEV (version) != GOMP_VERSION_HOST)
//    {
//        GOMP_PLUGIN_error ("Offload data incompatible with GCN plugin"
//                           " (expected %u, received %u)",
//                           GOMP_VERSION_GCN, GOMP_VERSION_DEV (version));
//        return false;
//    }

    addr_pair *pairs = (addr_pair *) target_data;
    void *shm_addr = (void *) pairs[0].start;
    size_t total_size = pairs[1].end - pairs[0].start;

    // Unmap the shared memory
    if (munmap(shm_addr, total_size) == -1) {
        GOMP_PLUGIN_error("munmap failed: %s", strerror(errno));
        return false;
    }

    // Optionally, i can remove the shared memory object name with shm_unlink("/my_shm");
    // But i need to be sure no other process needs it
    char shm_name[256];
    snprintf(shm_name, sizeof(shm_name), "/my_shm_%d", n);
    if (shm_unlink(shm_name) == -1) {
        GOMP_PLUGIN_error("shm_unlink failed: %s", strerror(errno));
        return false;
    }

    free(pairs);

    return true;
}


void *
GOMP_OFFLOAD_alloc(int n, size_t size)
{
    if (n < 0 || n >= MAX_DEVICES) {
        GOMP_PLUGIN_error ("Invalid device number %d\n", n);
        return NULL;
    }

    simulated_device *device = &devices[n];
    if (!device->initialized) {
        GOMP_PLUGIN_error("Attempt to allocate memory on uninitialized device %d\n", n);
        return NULL;
    }

    // Here i allocate anonymous memory, because (in my understanding) in 'GOMP_OFFLOAD_load_image' i allocate memory that i am intending to share
    // and in 'GOMP_OFFLOAD_alloc' i allocate memory that is supposed to be only for this process.
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        GOMP_PLUGIN_error("Memory allocation failed");
        return NULL;
    }

    pthread_mutex_lock(&device->lock);
    int i;
    for (i = 0; i < MAX_ALLOCATIONS; i++) {
        if (device->allocations[i].ptr == NULL) {
            device->allocations[i].ptr = ptr;
            device->allocations[i].size = size;
            break;
        }
    }
    pthread_mutex_unlock(&device->lock);

    // Here we check if the allocations array was full
    if (i == MAX_ALLOCATIONS) {
        GOMP_PLUGIN_error("Exceeded maximum allocation limit on device %d\n", n);
        munmap(ptr, size); // Cleanup newly allocated memory
        return NULL;
    }

    return ptr;
}



// Here i have a problem... i need to have additional parameter -- size,
// to understand how much data to clear
bool
GOMP_OFFLOAD_free(int n, void *ptr /*, size_t size*/)
{
    if (n < 0 || n >= MAX_DEVICES) {
        GOMP_PLUGIN_error ("Invalid device number %d\n", n);
        return false;
    }

    simulated_device *device = &devices[n];
    if (!device->initialized) {
        GOMP_PLUGIN_error("Attempt to free memory on uninitialized device %d\n", n);
        return false;
    }

    pthread_mutex_lock(&device->lock);
    bool found = false;
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (device->allocations[i].ptr == ptr) {
            // Free the memory using the recorded size
            if (munmap(ptr, device->allocations[i].size) == -1) {
                GOMP_PLUGIN_error("Memory deallocation failed\n");
                pthread_mutex_unlock(&device->lock);
                return false;
            }

            device->allocations[i].ptr = NULL;
            device->allocations[i].size = 0;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&device->lock);

    if (!found) {
        GOMP_PLUGIN_error("Pointer not recognized or already freed for device %d\n", n);
        return false;
    }

    return true;
}

bool
is_valid_memory(simulated_device *dev, void *ptr, size_t size)
{
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        void *start = dev->allocations[i].ptr;
        size_t len = dev->allocations[i].size;
        if (ptr >= start && (char *)ptr + size <= (char *)start + len) {
            return true;
        }
    }
    return false;
}


bool
GOMP_OFFLOAD_dev2host(int device, void *dst, const void *src, size_t n)
{
    if (device < 0 || device >= MAX_DEVICES) {
        GOMP_PLUGIN_error ("Invalid device number %d\n", n);
        return false;
    }

    simulated_device *dev = &devices[device];
    if (!dev->initialized) {
        GOMP_PLUGIN_error("Attempt to copy from an uninitialized device %d\n", device);
        return false;
    }

    if (!is_valid_memory(dev, src, n) || !is_valid_memory(dev, dst, n)) {
        GOMP_PLUGIN_error("Memory bounds violation during device to host copy\n");
        return false;
    }

    // Perform the memory copy
    memcpy(dst, src, n);
    HOST_PROCESS_DEBUG("Copying %zu bytes from device %d (%p) to host (%p)\n", n, device, src, dst);
    return true;
}


bool
GOMP_OFFLOAD_host2dev(int device, void *dst, const void *src, size_t n)
{
    if (device < 0 || device >= MAX_DEVICES) {
        GOMP_PLUGIN_error ("Invalid device number %d\n", n);
        return false;
    }

    simulated_device *dev = &devices[device];
    if (!dev->initialized) {
        GOMP_PLUGIN_error("Attempt to copy to an uninitialized device %d\n", device);
        return false;
    }

    if (!is_valid_memory(dev, dst, n) || !is_valid_memory(dev, src, n)) {
        GOMP_PLUGIN_error("Memory bounds violation during host to device copy\n");
        return false;
    }

    // Perform the memory copy
    memcpy(dst, src, n);
    HOST_PROCESS_DEBUG("Copying %zu bytes from host (%p) to device %d (%p)\n", n, src, device, dst);
    return true;
}



/// OPTIONAL
//extern int GOMP_OFFLOAD_memcpy2d (int, int, size_t, size_t,
//                                  void*, size_t, size_t, size_t,
//                                  const void*, size_t, size_t, size_t);
//extern int GOMP_OFFLOAD_memcpy3d (int, int, size_t, size_t, size_t, void *,
//                                  size_t, size_t, size_t, size_t, size_t,
//                                  const void *, size_t, size_t, size_t, size_t,
//                                  size_t);



//extern bool GOMP_OFFLOAD_dev2dev (int, void *, const void *, size_t);
//extern bool GOMP_OFFLOAD_can_run (void *);
//extern void GOMP_OFFLOAD_run (int, void *, void *, void **);
//extern void GOMP_OFFLOAD_async_run (int, void *, void *, void **, void *);
//
//extern void GOMP_OFFLOAD_openacc_exec (void (*) (void *), size_t, void **,
//                                       void **, unsigned *, void *);
//extern void *GOMP_OFFLOAD_openacc_create_thread_data (int);
//
//
//
//void
//GOMP_OFFLOAD_openacc_destroy_thread_data (void *data)
//{
//    free (data);
//}
//
//
//
//extern struct goacc_asyncqueue *GOMP_OFFLOAD_openacc_async_construct (int);
//extern bool GOMP_OFFLOAD_openacc_async_destruct (struct goacc_asyncqueue *);
//extern int GOMP_OFFLOAD_openacc_async_test (struct goacc_asyncqueue *);
//extern bool GOMP_OFFLOAD_openacc_async_synchronize (struct goacc_asyncqueue *);
//extern bool GOMP_OFFLOAD_openacc_async_serialize (struct goacc_asyncqueue *,
//                                                  struct goacc_asyncqueue *);
//extern void GOMP_OFFLOAD_openacc_async_queue_callback (struct goacc_asyncqueue *,
//                                                       void (*)(void *), void *);
//extern void GOMP_OFFLOAD_openacc_async_exec (void (*) (void *), size_t, void **,
//                                             void **, unsigned *, void *,
//                                             struct goacc_asyncqueue *);
//extern bool GOMP_OFFLOAD_openacc_async_dev2host (int, void *, const void *, size_t,
//                                                 struct goacc_asyncqueue *);
//extern bool GOMP_OFFLOAD_openacc_async_host2dev (int, void *, const void *, size_t,
//                                                 struct goacc_asyncqueue *);
//extern void *GOMP_OFFLOAD_openacc_cuda_get_current_device (void);
//extern void *GOMP_OFFLOAD_openacc_cuda_get_current_context (void);
//extern void *GOMP_OFFLOAD_openacc_cuda_get_stream (struct goacc_asyncqueue *);
//extern int GOMP_OFFLOAD_openacc_cuda_set_stream (struct goacc_asyncqueue *,
//                                                 void *);
//extern union goacc_property_value
//GOMP_OFFLOAD_openacc_get_property (int, enum goacc_property);
