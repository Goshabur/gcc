/* Host process plugin */

#pragma GCC optimize ("O0")

#include <sys/wait.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>

#include "config.h"
#include "symcat.h"
#include "libgomp-plugin.h"
#include "oacc-plugin.h"
#include "gomp-constants.h"
#include "oacc-int.h"
#include <errno.h>

#define MAX_DEVICES 1
#define MAX_ALLOCATIONS 100
//#define HOST_PROCESS_DEBUG(...) GOMP_PLUGIN_debug("HOST_PROCESS debug: ", __VA_ARGS__)

const char *
GOMP_OFFLOAD_get_name (void)
{
    return "host_process";
}

unsigned int
GOMP_OFFLOAD_get_caps (void)
{
    return GOMP_OFFLOAD_CAP_NATIVE_EXEC | GOMP_OFFLOAD_CAP_OPENMP_400;
}

int
GOMP_OFFLOAD_get_type (void)
{
    return OFFLOAD_TARGET_TYPE_HOST_PROCESS;
}

int
GOMP_OFFLOAD_get_num_devices (unsigned int omp_requires_mask)
{
    return 1;
}

bool
call_function_with_args(void *fn_ptr, void *vars)
{
    return false;
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
    int socket_fd;
    allocation allocations[MAX_ALLOCATIONS];
    void *image_memory;
    size_t image_size;
} simulated_device;

// Assuming i have an array of devices
simulated_device devices[MAX_DEVICES];


bool
wrong_device_number(int n)
{
    if (n < 0 || n >= MAX_DEVICES) {
        GOMP_PLUGIN_error ("Invalid device number %d\n", n);
        return true;
    }
    return false;
}

bool
check_device_validity(int n)
{
    if (wrong_device_number(n)) {
        return false;
    }

    simulated_device *device = &devices[n];
    if (!device->initialized || device->socket_fd < 0) {
        GOMP_PLUGIN_error("Device %d not initialized or socket not valid\n", n);
        return false;
    }
    return true;
}


void
handle_alloc(int n, size_t size)
{
    simulated_device *device = &devices[n];
    int index = -1;

    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (device->allocations[i].ptr == NULL) {
            index = i;
            break;
        }
    }

    void *ptr = NULL;
    if (index != -1) {
        ptr = malloc(size);
        if (ptr) {
            device->allocations[index].ptr = ptr;
            device->allocations[index].size = size;
        }
    }

    send(device->socket_fd, &ptr, sizeof(ptr), 0);
}


void
handle_free(int n, const char *command_details)
{
    simulated_device *device = &devices[n];
    void *ptr;

    int parsed = sscanf(command_details, "%p", &ptr);
    if (parsed != 1) {
        GOMP_PLUGIN_error("Failed to parse FREE command details\n");
        send(device->socket_fd, "ERROR", 5, 0);
        return;
    }

    free(ptr);
    send(device->socket_fd, "OK", 2, 0);


//// If i am tracking allocations in 'allocations' array.
//    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
//        if (device->allocations[i].ptr == ptr) {
//            free(ptr);
//            device->allocations[i].ptr = NULL;
//            device->allocations[i].size = 0;
//            break;
//        }
//    }
}

void
handle_load_image(int n)
{
    simulated_device *device = &devices[n];
    size_t total_size;

    int bytes_received = recv(device->socket_fd, &total_size, sizeof(total_size), 0);
    if (bytes_received < sizeof(total_size)) {
        GOMP_PLUGIN_error("Failed to receive total data size\n");
        return;
    }

    void *image_memory = malloc(total_size);
    if (!image_memory) {
        GOMP_PLUGIN_error("Failed to allocate memory for image\n");
        return;
    }

    size_t received = 0;
    while (received < total_size) {
        char *buffer = ((char *)image_memory) + received;
        size_t to_receive = total_size - received;
        bytes_received = recv(device->socket_fd, buffer, to_receive, 0);
        if (bytes_received <= 0) {
            GOMP_PLUGIN_error("Failed to receive image data\n");
            free(image_memory);
            return;
        }
        received += bytes_received;
    }

    // Optionally, but i decided to store the image memory and size for possible later reference
    // ***And yeah, here i assume that a single image load per device***
    //// It appears not to be true, so multiple images can be loaded on the device at the same time
    device->image_memory = image_memory;
    device->image_size = total_size;

    send(device->socket_fd, &image_memory, sizeof(image_memory), 0);
}

void
handle_unload_image(int n)
{
    simulated_device *device = &devices[n];

    if (device->image_memory == NULL) {
        GOMP_PLUGIN_error("No image is currently loaded on device %d\n", n);
        char response[64] = "ERROR, No image loaded";
        send(device->socket_fd, response, strlen(response), 0);
        return;
    }

    free(device->image_memory);
    device->image_memory = NULL;
    device->image_size = 0;

    GOMP_PLUGIN_debug(0, "Image successfully unloaded from device %d\n", n);

    char response[64] = "OK, Image unloaded";
    send(device->socket_fd, response, strlen(response), 0);
}

void
handle_host2dev(int n, const char *command_details)
{
    simulated_device *device = &devices[n];
    void *dst;
    size_t size;

    int parsed = sscanf(command_details, "%p %zu", &dst, &size);
    if (parsed != 2) {
        GOMP_PLUGIN_error("Failed to parse HOST_TO_DEV command details\n");
        send(device->socket_fd, "ERROR", 5, 0);
        return;
    }

    char *data = malloc(size);
    if (!data) {
        GOMP_PLUGIN_error("Memory allocation failed for data transfer\n");
        send(device->socket_fd, "ERROR", 5, 0);
        return;
    }

    int bytes_received = recv(device->socket_fd, data, size, 0);
    if (bytes_received < (int)size) {
        GOMP_PLUGIN_error("Failed to receive all data\n");
        free(data);
        send(device->socket_fd, "ERROR", 5, 0);
        return;
    }

    memcpy(dst, data, size);
    free(data);
    send(device->socket_fd, "OK", 2, 0);
}

void
handle_dev2host(int n, const char *command_details)
{
    simulated_device *device = &devices[n];
    void *src;
    size_t size;

    int parsed = sscanf(command_details, "%p %zu", &src, &size);
    if (parsed != 2) {
        GOMP_PLUGIN_error("Failed to parse DEV_TO_HOST command details");
        size_t error_indicator = 0; // Here i want to send a specific error code
        send(device->socket_fd, &error_indicator, sizeof(size_t), 0);
        return;
    }

    // Can i assume here that src points to valid memory in this process ?
    if (send(device->socket_fd, src, size, 0) < size) {
        GOMP_PLUGIN_error("Failed to send all data to host");
    }
}

void handle_dev2dev(int n, const char *command_details) {
    simulated_device *device = &devices[n];
    void *dst, *src;
    size_t size;

    int parsed = sscanf(command_details, "%p %p %zu", &dst, &src, &size);
    if (parsed != 3) {
        GOMP_PLUGIN_error("Failed to parse DEV_TO_DEV command details");
        send(device->socket_fd, "ERROR", 5, 0);
        return;
    }

    memcpy(dst, src, size);
    send(device->socket_fd, "OK", 2, 0);
}



void
handle_call_function(int n, const char *command_details)
{
    simulated_device *device = &devices[n];
    void *fn_ptr;
    void *vars;

    int parsed = sscanf(command_details, "%p %p", &fn_ptr, &vars);
    if (parsed != 2) {
        GOMP_PLUGIN_error("Failed to parse CALL_FN command details\n");
        send(device->socket_fd, "ERROR", 5, 0);
        return;
    }

    if(call_function_with_args(fn_ptr, vars)) {
        send(device->socket_fd, "OK", 2, 0);
    } else {
        send(device->socket_fd, "ERROR", 5, 0);
    }
}

void
child_process(int n)
{
    simulated_device *device = &devices[n];
    char buffer[1024];

    while (1) {
        int bytes_received = recv(device->socket_fd, buffer, sizeof(buffer), 0);
        if (bytes_received == 0) { // connection was closed by the other side
            break;
        } else if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  // no data available right now, wait and try again
            } else {
                perror("recv failed");
                break;
            }
        }

        buffer[bytes_received] = '\0';
        if (strncmp(buffer, "ALLOC", 5) == 0) {
            size_t size = atoi(buffer + 6);
            handle_alloc(n, size);
        } else if (strncmp(buffer, "FREE", 4) == 0) {
            handle_free(n, buffer + 5);
        } else if (strcmp(buffer, "LOAD_IMAGE") == 0) {
            handle_load_image(n);
        } else if (strcmp(buffer, "UNLOAD_IMAGE") == 0) {
            handle_unload_image(n);
        } else if (strncmp(buffer, "HOST_TO_DEV", 11) == 0) {
            handle_host2dev(n, buffer + 12);
        } else if (strncmp(buffer, "DEV_TO_HOST", 11) == 0) {
            handle_dev2host(n, buffer + 12);
        } else if (strncmp(buffer, "CALL_FN", 7) == 0) {
            handle_call_function(n, buffer + 8);
        } else if (strncmp(buffer, "DEV_TO_DEV", 10) == 0) {
            handle_dev2dev(n, buffer + 11);
        }
    }
    close(device->socket_fd);
}

bool validate_socket_connection(int socket_fd) {
    if (socket_fd < 0) {
        GOMP_PLUGIN_error("Invalid socket descriptor.\n");
        return false;
    }

    int error = 0;
    socklen_t errlen = sizeof(error);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &errlen) != 0) {
        GOMP_PLUGIN_error("Socket operation failed.\n");
        return false;
    }

    if (error != 0) {
        GOMP_PLUGIN_error("Socket error: %s\n", strerror(error));
        return false;
    }

    return true;
}


bool
GOMP_OFFLOAD_init_device(int n)
{
    if (wrong_device_number(n)) {
        return false;
    }

    simulated_device *device = &devices[n];
    if (device->initialized) {
        return true;
    }

    // Initialize a mutex for this device
    if (pthread_mutex_init(&device->lock, NULL) != 0) {
        GOMP_PLUGIN_error("Failed to initialize a mutex for simulated device %d\n", n);
        return false;
    }

    int sockets[2];  // Socket pair
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        GOMP_PLUGIN_error("Failed to create socket pair");
        pthread_mutex_destroy(&device->lock);
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
        GOMP_PLUGIN_error("Failed to fork a simulated device process");
        pthread_mutex_destroy(&device->lock);
        close(sockets[0]);
        close(sockets[1]);
        return false;
    } else if (pid == 0) {
        close(sockets[0]);
        device->socket_fd = sockets[1];  // here i save the parent's socket descriptor
        device->initialized = true;
        child_process(n);  // function for child process
        _exit(0);
    }

    // Parent process
    close(sockets[1]);
    device->socket_fd = sockets[0];  // here i save the parent's socket descriptor
    device->process_id = pid;
    device->initialized = true;
    GOMP_PLUGIN_debug(0, "Simulated device %d initialized with process ID %d and socket FD %d\n", n, pid, sockets[0]);

    return true;
}


bool
GOMP_OFFLOAD_fini_device(int n)
{
    if (wrong_device_number(n)) {
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
    close(device->socket_fd);
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
//    if (GOMP_VERSION_DEV (version) != GOMP_VERSION_HOST_PROCESS)
//    {
//        GOMP_PLUGIN_error ("Offload data incompatible with HOST_PROCESS plugin"
//                           " (expected %u, received %u)",
//                           GOMP_VERSION_HOST_PROCESS, GOMP_VERSION_DEV (version));
//        return false;
//    }

    if(!check_device_validity(n)) return false;
    simulated_device *device = &devices[n];

    host_process_image_desc *img = (host_process_image_desc *)target_data;
    size_t total_size = img->code_size + img->data_size;

////    pthread_mutex_lock(&device->lock);
    char command[256] = "LOAD_IMAGE";
    if (send(device->socket_fd, command, strlen(command), 0) < 0) {
        GOMP_PLUGIN_error("Failed to send load command\n");
////        pthread_mutex_unlock(&device->lock);
        return false;
    }

    // i want to send the size of the code+data first
    if (send(device->socket_fd, &total_size, sizeof(total_size), 0) < 0) {
        GOMP_PLUGIN_error("Failed to send data size\n");
////        pthread_mutex_unlock(&device->lock);
        return -1;
    }

    // Send the code
    if (send(device->socket_fd, img->code, img->code_size, 0) < 0) {
        GOMP_PLUGIN_error("Failed to send code data\n");
////        pthread_mutex_unlock(&device->lock);
        return -1;
    }

    // Send the data
    if (send(device->socket_fd, img->data, img->data_size, 0) < 0) {
        GOMP_PLUGIN_error("Failed to send image data\n");
////        pthread_mutex_unlock(&device->lock);
        return -1;
    }

    // Allocate the target_table with 2 entries: one for code, one for data
    *target_table = malloc(2 * sizeof(struct addr_pair));
    if (*target_table == NULL) {
        GOMP_PLUGIN_error("malloc failed");
        return -1;
    }

    void *response_address;
    int recv_size = recv(device->socket_fd, &response_address, sizeof(response_address), 0);
    if (recv_size < sizeof(response_address)) {
        GOMP_PLUGIN_error("Failed to receive response from child process\n");
////        pthread_mutex_unlock(&device->lock);
        return -1;
    }

    // Set up the address pairs for code and data
    (*target_table)[0].start = (uintptr_t) response_address;
    (*target_table)[0].end = (uintptr_t) response_address + img->code_size;
    (*target_table)[1].start = (uintptr_t) (response_address + img->code_size);
    (*target_table)[1].end = (uintptr_t) (response_address + img->code_size + img->data_size);

    return 0;
}

bool
GOMP_OFFLOAD_unload_image(int n, unsigned version, const void *target_data)
{
//    if (GOMP_VERSION_DEV (version) != GOMP_VERSION_HOST_PROCESS)
//    {
//        GOMP_PLUGIN_error ("Offload data incompatible with HOST_PROCESS plugin"
//                           " (expected %u, received %u)",
//                           GOMP_VERSION_HOST_PROCESS, GOMP_VERSION_DEV (version));
//        return false;
//    }

    if(!check_device_validity(n)) return false;
    simulated_device *device = &devices[n];

    char command[256] = "UNLOAD_IMAGE";
    if (send(device->socket_fd, command, strlen(command), 0) < 0) {
        GOMP_PLUGIN_error("Failed to send unload command\n");
        return false;
    }

    char response[64];
    int bytes_received = recv(device->socket_fd, response, sizeof(response), 0);
    if (bytes_received <= 0) {
        GOMP_PLUGIN_error("Failed to receive confirmation from device about unload operation");
        return false;
    }
    GOMP_PLUGIN_debug(0, "%s", response);

    return true;
}

// What pointer should 'GOMP_OFFLOAD_alloc' return, if i am allocating in another address space ?..
void *
GOMP_OFFLOAD_alloc(int n, size_t size)
{
    if(!check_device_validity(n)) return NULL;
    simulated_device *device = &devices[n];

////    pthread_mutex_lock(&device->lock);
    if (!validate_socket_connection(device->socket_fd)) {
        GOMP_PLUGIN_error("Attempting to reconnect...\n");
        // Reconnect or reinitialize the socket
    }

    // Here i construct and send an allocation command
    char command[256];
    sprintf(command, "ALLOC %zu", size);
    int bytes_sent = send(device->socket_fd, command, strlen(command), 0);
    if (bytes_sent < 0) {
        GOMP_PLUGIN_error("Failed to send allocation command\n");
////        pthread_mutex_unlock(&device->lock);
        return NULL;
    }

    // Seems like here i am expecting a response with the address of the allocated memory
    void *ptr;
    int recv_size = recv(device->socket_fd, &ptr, sizeof(ptr), 0);
    if (recv_size < sizeof(ptr)) {
        GOMP_PLUGIN_error("Failed to receive allocation response or allocation failed\n");
////        pthread_mutex_unlock(&device->lock);
        return NULL;
    }

////    pthread_mutex_unlock(&device->lock);
    return ptr; //// in process 'A' i am returning pointer from the address space of another process...
}


bool
GOMP_OFFLOAD_free(int n, void *ptr)
{
    if(!check_device_validity(n)) return false;
    simulated_device *device = &devices[n];

//    pthread_mutex_lock(&device->lock);
    bool found = false;
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (device->allocations[i].ptr == ptr) {
            char command[256];
            sprintf(command, "FREE %p", ptr);
            if (send(device->socket_fd, command, strlen(command), 0) < 0) {
                GOMP_PLUGIN_error("Failed to send free command\n");
////                pthread_mutex_unlock(&device->lock);
                return false;
            }

            found = true;
            break;
        }
    }
//    pthread_mutex_unlock(&device->lock);
    if (!found) {
        GOMP_PLUGIN_error("Pointer not recognized or already freed for device %d\n", n);
        return false;
    }

    char response[64];
    int bytes_received = recv(device->socket_fd, response, sizeof(response), 0);
    if (bytes_received <= 0) {
        GOMP_PLUGIN_error("Failed to receive confirmation from device about free operation");
        return false;
    }
    GOMP_PLUGIN_debug(0, "%s", response);

    return true;
}


//// THIS FUNCTION CHECKS THAT 'ptr' is in the memory that was allocated
//bool
//is_valid_memory(simulated_device *dev, void *ptr, size_t size)
//{
//    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
//        void *start = dev->allocations[i].ptr;
//        size_t len = dev->allocations[i].size;
//        if (ptr >= start && (char *)ptr + size <= (char *)start + len) {
//            return true;
//        }
//    }
//    return false;
//}


bool
GOMP_OFFLOAD_dev2host(int n, void *dst, const void *src, size_t size)
{
    if(!check_device_validity(n)) return false;
    simulated_device *device = &devices[n];

    char command[64];
    snprintf(command, sizeof(command), "DEV_TO_HOST %p %zu", src, size);
    if (send(device->socket_fd, command, strlen(command), 0) < 0) {
        GOMP_PLUGIN_error("Failed to send DEV_TO_HOST command to device");
        return false;
    }

    // Receive the data
    int bytes_received = recv(device->socket_fd, dst, size, 0);
    if (bytes_received < (int)size) {
        GOMP_PLUGIN_error("Failed to receive all data from device");
        return false;
    }

    return true;
}


bool
GOMP_OFFLOAD_host2dev(int n, void *dst, const void *src, size_t size)
{
    if(!check_device_validity(n)) return false;
    simulated_device *device = &devices[n];

    char command[64];
    snprintf(command, sizeof(command), "HOST_TO_DEV %p %zu", dst, size);
    if (send(device->socket_fd, command, strlen(command), 0) < 0) {
        GOMP_PLUGIN_error("Failed to send HOST_TO_DEV command to device");
        return false;
    }

    if (send(device->socket_fd, src, size, 0) < 0) {
        GOMP_PLUGIN_error("Failed to send data to device");
        return false;
    }

    char response[64];
    if (recv(device->socket_fd, response, sizeof(response), 0) <= 0) {
        GOMP_PLUGIN_error("Failed to receive confirmation from device about successful copy from host to device");
        return false;
    }

    if (strcmp(response, "OK") != 0) {
        GOMP_PLUGIN_error("Device reported an error during data transfer");
        return false;
    }

    return true;
}

void
GOMP_OFFLOAD_run (int n, void *fn_ptr, void *vars, void **args)
{
    if(!check_device_validity(n)) return;
    simulated_device *device = &devices[n];

    char command[64];
    snprintf(command, sizeof(command), "CALL_FN %p %p", fn_ptr, vars);
    if (send(device->socket_fd, command, strlen(command), 0) < 0) {
        GOMP_PLUGIN_error("Failed to send CALL_FN command to device");
        return;
    }

    char response[64];
    if (recv(device->socket_fd, response, sizeof(response), 0) <= 0) {
        GOMP_PLUGIN_error("Failed to receive confirmation from device about successful function call");
        return;
    }
    GOMP_PLUGIN_debug(0, "%s", response);
}



bool
GOMP_OFFLOAD_dev2dev (int n, void *dst, const void *src, size_t size)
{
    if(!check_device_validity(n)) return false;
    simulated_device *device = &devices[n];

    char command[64];
    snprintf(command, sizeof(command), "DEV_TO_DEV %p %p %zu", dst, src, size);
    if (send(device->socket_fd, command, strlen(command), 0) < 0) {
        GOMP_PLUGIN_error("Failed to send DEV_TO_DEV command to device");
        return false;
    }

    char response[64];
    if (recv(device->socket_fd, response, sizeof(response), 0) <= 0) {
        GOMP_PLUGIN_error("Failed to receive confirmation from device about successful copy");
        return false;
    }

    if (strcmp(response, "OK") != 0) {
        GOMP_PLUGIN_error("Device reported an error during data copy");
        return false;
    }

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
