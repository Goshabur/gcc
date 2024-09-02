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
#define ASYNC_QUEUE_SIZE 64
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
    void (*fn) (void *) = fn_ptr;
    fn(vars);
    return true;
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
    struct goacc_asyncqueue *async_queues;
    pthread_mutex_t async_queues_mutex;
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
handle_alloc(int n, const char *command_details)
{
    simulated_device *device = &devices[n];
    size_t size;
    int parsed = sscanf(command_details, "%zu", &size);
    if (parsed != 1) {
        GOMP_PLUGIN_error("Failed to parse ALLOC command details\n");
        send(device->socket_fd, "ERROR", 5, 0);
        return;
    }

    void *ptr = malloc(size);
    send(device->socket_fd, &ptr, sizeof(ptr), 0);

//    int index = -1;
//    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
//        if (device->allocations[i].ptr == NULL) {
//            index = i;
//            break;
//        }
//    }
//
//    void *ptr = NULL;
//    if (index != -1) {
//        ptr = malloc(size);
//        if (ptr) {
//            device->allocations[index].ptr = ptr;
//            device->allocations[index].size = size;
//        }
//    }
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
handle_load_image(int n, char *command_details)
{

    simulated_device *device = &devices[n];
    size_t size;

    int parsed = sscanf(command_details, "%zu", &size);
    if (parsed != 1) {
        GOMP_PLUGIN_error("Failed to parse LOAD_IMAGE command details\n");
        send(device->socket_fd, "ERROR", 5, 0);
        return;
    }
    size_t prefix_length = snprintf(NULL, 0, "%zu ", size);
    void *image_memory = malloc(size);
    if (!image_memory) {
        GOMP_PLUGIN_error("Failed to allocate memory for image\n");
        return;
    }
    image_memory = command_details + prefix_length;

    device->image_memory = image_memory;
    device->image_size = size;
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
handle_host2dev(int n, const char *command_details, const char *data_end)
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
    size_t prefix_length = snprintf(NULL, 0, "%p %zu ", dst, size);
    memcpy(dst, command_details + prefix_length, size);
    send(device->socket_fd, "OK", 2, 0);

//    const char *data_start = command_details;
//    for (int i = 0; i < 2; i++) {
//        data_start = strchr(data_start, ' ');
//        if (!data_start) {
//            GOMP_PLUGIN_error("Failed to locate data start in command\n");
//            send(device->socket_fd, "ERROR", 5, 0);
//            return;
//        }
//        data_start++;
//    }



//    const char *data = command_details + data_offset;
//
//    char *data = malloc(size);
//    if (!data) {
//        GOMP_PLUGIN_error("Memory allocation failed for data transfer\n");
//        send(device->socket_fd, "ERROR", 5, 0);
//        return;
//    }
//
//    int bytes_received = recv(device->socket_fd, data, size, 0);
//    if (bytes_received < (int)size) {
//        GOMP_PLUGIN_error("Failed to receive all data\n");
//        free(data);
//        send(device->socket_fd, "ERROR", 5, 0);
//        return;
//    }
//    free(data);

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

//    void (*actual_function_pointer)(int, int) = (void(*)(int, int))fn_ptr;
//    actual_function_pointer(vars);

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
    char *buffer = malloc(1 << 20);
//    int pointer = 0;

    while (1) {
        size_t message_length;
        size_t bytes_received = recv(device->socket_fd, &message_length, sizeof(message_length), 0);
        if (bytes_received < sizeof(message_length)) {
            printf("Failed to receive message length.\n");
            return;
        }

        bytes_received = recv(device->socket_fd, buffer, message_length, 0);
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

        size_t remaining = message_length;
        remaining -= bytes_received;
        while (remaining > 0) {
            bytes_received = recv(device->socket_fd, buffer + bytes_received, remaining, 0);
//            error handling
            remaining -= bytes_received;
        }


        buffer[bytes_received] = '\0';
        if (strncmp(buffer, "ALLOC", 5) == 0) {
            handle_alloc(n, buffer + 6);
        } else if (strncmp(buffer, "FREE", 4) == 0) {
            handle_free(n, buffer + 5);
        } else if (strncmp(buffer, "LOAD_IMAGE", 10) == 0) {
            handle_load_image(n, buffer + 11);
        } else if (strncmp(buffer, "UNLOAD_IMAGE", 12) == 0) {
            handle_unload_image(n);
        } else if (strncmp(buffer, "HOST_TO_DEV", 11) == 0) {
            handle_host2dev(n, buffer + 12, &buffer[bytes_received]);
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

bool
send_size_and_data(int n, char* command_name, char* command, size_t command_size)
{
    simulated_device *device = &devices[n];
    int bytes_sent = send(device->socket_fd, &command_size, sizeof(command_size), 0);
    if (bytes_sent < 0) {
        GOMP_PLUGIN_error("Failed to send command size\n");
        return false;
    }

    bytes_sent = send(device->socket_fd, command, command_size, 0);
    if (bytes_sent < 0) {
        GOMP_PLUGIN_error("Failed to send %s command and data to device\n", command_name);
        return false;
    }
    return true;
}

bool
validate_socket_connection(int socket_fd)
{
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
        GOMP_PLUGIN_error("Failed to create socket pair\n");
        pthread_mutex_destroy(&device->lock);
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
        GOMP_PLUGIN_error("Failed to fork a simulated device process\n");
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
            GOMP_PLUGIN_error("Failed to terminate simulated device process\n");
            return false;
        }

        // Wait for the process to terminate
        int status;
        if (waitpid(device->process_id, &status, 0) == -1) {
            GOMP_PLUGIN_error("Failed to wait for simulated device process to terminate\n");
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

//    host_process_image_desc *img = (host_process_image_desc *)target_data;
//    size_t total_size = img->code_size + img->data_size;
    const size_t data_size = 0;

    int prefix_len = snprintf(NULL, 0, "LOAD_IMAGE %zu ", data_size);
    size_t total_length = prefix_len + data_size;

    char *buffer = malloc(total_length);
    if (!buffer) {
        GOMP_PLUGIN_error("Failed to allocate buffer for command and data\n");
        return false;
    }

    int written = sprintf(buffer, "LOAD_IMAGE %zu ", data_size);
    memcpy(buffer + written, target_data, data_size);
    if(!send_size_and_data(n, "LOAD_IMAGE", buffer, total_length)){
        free(buffer);
        return false;
    }
    free(buffer);

    void *response_address;
    int recv_size = recv(device->socket_fd, &response_address, sizeof(response_address), 0);
    if (recv_size < sizeof(response_address)) {
        GOMP_PLUGIN_error("Failed to receive response from child process\n");
        return -1;
    }

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

    char command[256];
    size_t message_length = sprintf(command, "UNLOAD_IMAGE");
    if(!send_size_and_data(n, "UNLOAD_IMAGE", command, message_length)){
        return false;
    }

    char response[64];
    int bytes_received = recv(device->socket_fd, response, sizeof(response), 0);
    if (bytes_received <= 0) {
        GOMP_PLUGIN_error("Failed to receive confirmation from device about unload operation\n");
        return false;
    }
    GOMP_PLUGIN_debug(0, "%s\n", response);

    return true;
}

// What pointer should 'GOMP_OFFLOAD_alloc' return, if i am allocating in another address space ?..
void *
GOMP_OFFLOAD_alloc(int n, size_t size)
{
    if(!check_device_validity(n)) return NULL;
    simulated_device *device = &devices[n];

////    pthread_mutex_lock(&device->lock);
//    if (!validate_socket_connection(device->socket_fd)) {
//        GOMP_PLUGIN_error("Attempting to reconnect...\n");
//        // Reconnect or reinitialize the socket
//    }

    char command[256];
    size_t message_length = sprintf(command, "ALLOC %zu", size);
    if(!send_size_and_data(n, "ALLOC", command, message_length)){
        return NULL;
    }

    // Seems like here i am expecting a response with the address of the allocated memory
    void *ptr;
    int bytes_received = recv(device->socket_fd, &ptr, sizeof(ptr), 0);
    if (bytes_received < sizeof(ptr)) {
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
//    bool found = false;
//    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
//        if (device->allocations[i].ptr == ptr) {
//            char command[256];
//            sprintf(command, "FREE %p", ptr);
//            if (send(device->socket_fd, command, strlen(command), 0) < 0) {
//                GOMP_PLUGIN_error("Failed to send free command\n");
//////                pthread_mutex_unlock(&device->lock);
//                return false;
//            }
//
//            found = true;
//            break;
//        }
//    }
////    pthread_mutex_unlock(&device->lock);
//    if (!found) {
//        GOMP_PLUGIN_error("Pointer not recognized or already freed for device %d\n", n);
//        return true;
//    }

    char command[256];
    size_t message_length = sprintf(command, "FREE %p", ptr);
    if(!send_size_and_data(n, "FREE", command, message_length)){
        return false;
    }

    char response[64];
    int bytes_received = recv(device->socket_fd, response, sizeof(response), 0);
    if (bytes_received <= 0) {
        GOMP_PLUGIN_error("Failed to receive confirmation from device about free operation\n");
        return false;
    }
    GOMP_PLUGIN_debug(0, "%s\n", response);

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

    char command[256];
    size_t message_length = sprintf(command,  "DEV_TO_HOST %p %zu", src, size);
    if(!send_size_and_data(n, "DEV_TO_HOST", command, message_length)){
        return false;
    }

    // Receive the data
    int bytes_received = recv(device->socket_fd, dst, size, 0);
    if (bytes_received < (int)size) {
        GOMP_PLUGIN_error("Failed to receive all data from device\n");
        return false;
    }

    return true;
}


bool
GOMP_OFFLOAD_host2dev(int n, void *dst, const void *src, size_t size)
{
    if(!check_device_validity(n)) return false;
    simulated_device *device = &devices[n];

    int prefix_len = snprintf(NULL, 0, "HOST_TO_DEV %p %zu ", dst, size);
    size_t total_length = prefix_len + size;

    char *buffer = malloc(total_length);
    if (!buffer) {
        GOMP_PLUGIN_error("Failed to allocate buffer for command and data\n");
        return false;
    }

    // Write the command, including the offset to the data, into the buffer
    int written = sprintf(buffer, "HOST_TO_DEV %p %zu ", dst, size);
    memcpy(buffer + written, src, size);

    if(!send_size_and_data(n, "HOST_TO_DEV", buffer, total_length)){
        free(buffer);
        return false;
    }
    free(buffer);

    char response[64];
    if (recv(device->socket_fd, response, sizeof(response), 0) <= 0) {
        GOMP_PLUGIN_error("Failed to receive confirmation from device about successful copy from host to device\n");
        return false;
    }

    if (strncmp(response, "OK", 2) != 0) {
        GOMP_PLUGIN_error("Device reported an error during data transfer\n");
        return false;
    }

    return true;
}

void
GOMP_OFFLOAD_run (int n, void *fn_ptr, void *vars, void **args)
{
    if(!check_device_validity(n)) return;
    simulated_device *device = &devices[n];

    char command[256];
    size_t message_length = sprintf(command, "CALL_FN %p %p", fn_ptr, vars);
    if(!send_size_and_data(n, "CALL_FN", command, message_length)){
        return;
    }

    char response[64];
    if (recv(device->socket_fd, response, sizeof(response), 0) <= 0) {
        GOMP_PLUGIN_error("Failed to receive confirmation from device about successful function call\n");
        return;
    }
    GOMP_PLUGIN_debug(0, "%s\n", response);
}



bool
GOMP_OFFLOAD_dev2dev (int n, void *dst, const void *src, size_t size)
{
    if(!check_device_validity(n)) return false;
    simulated_device *device = &devices[n];

    char command[256];
    size_t message_length = sprintf(command, "DEV_TO_DEV %p %p %zu", dst, src, size);
    if(!send_size_and_data(n, "DEV_TO_DEV", command, message_length)){
        return false;
    }

    char response[64];
    if (recv(device->socket_fd, response, sizeof(response), 0) <= 0) {
        GOMP_PLUGIN_error("Failed to receive confirmation from device about successful copy\n");
        return false;
    }

    if (strncmp(response, "OK", 2) != 0) {
        GOMP_PLUGIN_error("Device reported an error during data copy\n");
        return false;
    }

    return true;
}







////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////





void
GOMP_OFFLOAD_openacc_destroy_thread_data (void *data)
{
    free (data);
}


/* Execute an OpenACC kernel, synchronously or asynchronously.  */
static void
host_process_exec (void (*fn),
          void **devaddrs, unsigned *dims, void *targ_mem_desc, bool async,
          struct goacc_asyncqueue *aq)
{

}



/* Run a synchronous OpenACC kernel.  The device number is inferred from the
   already-loaded KERNEL.  */
void
GOMP_OFFLOAD_openacc_exec (void (*fn) (void *),
                           size_t mapnum  __attribute__((unused)),
                           void **hostaddrs __attribute__((unused)),
                           void **devaddrs,
                           unsigned *dims, void *targ_mem_desc)
{
    if (!fn) {
        GOMP_PLUGIN_error("Invalid kernel function pointer.\n");
        return;
    }
//    simulated_device *device = &devices[n];
//    if (!device || !device->initialized) {
//        GOMP_PLUGIN_error("Device not initialized or available.\n");
//        return;
//    }
    host_process_exec (fn, devaddrs, dims, targ_mem_desc, false, NULL);
}

void
GOMP_OFFLOAD_openacc_async_exec (void (*fn) (void *),
                                 size_t mapnum __attribute__((unused)),
                                 void **hostaddrs __attribute__((unused)),
                                 void **devaddrs,
                                 unsigned *dims, void *targ_mem_desc,
                                 struct goacc_asyncqueue *aq)
{
    if (!fn) {
        GOMP_PLUGIN_error("Invalid kernel function pointer.\n");
        return;
    }
    host_process_exec (fn, devaddrs, dims, targ_mem_desc, true, NULL);
}


void
initialize_async_queue(struct goacc_asyncqueue *aq);


typedef struct {
    void (*function)(void* data);
    void *data;
} queue_task;


/* A data struct for the copy_data callback.  */
struct copy_data
{
    int device;
    void *dst;
    const void *src;
    size_t len;
    struct goacc_asyncqueue *aq;
    bool host_to_device;
};


struct goacc_asyncqueue
{
    simulated_device *device;
//    hsa_queue_t *hsa_queue;

    pthread_t thread_drain_queue;
    pthread_mutex_t mutex;
    pthread_cond_t queue_cond_in;
    pthread_cond_t queue_cond_out;
    queue_task queue[ASYNC_QUEUE_SIZE]; // queue_task should be universal, but now it is only for copy operations.
    int queue_first;
    int queue_n;
    int drain_queue_stop;

    int id;
    struct goacc_asyncqueue *prev;
    struct goacc_asyncqueue *next;
};

void
data_copy_function(void *data) // This function will handle the data copying
{
    struct copy_data *copy_data = (struct copy_data *)data;
    if(copy_data->host_to_device) {
        GOMP_OFFLOAD_host2dev(copy_data->device, copy_data->dst, copy_data->src, copy_data->len);
    } else {
        GOMP_OFFLOAD_dev2host(copy_data->device, copy_data->dst, copy_data->src, copy_data->len);
    }
}

void
push_async_task(struct goacc_asyncqueue *aq, queue_task task)
{
    pthread_mutex_lock(&aq->mutex);
    while (aq->queue_n >= ASYNC_QUEUE_SIZE) {
        pthread_cond_wait(&aq->queue_cond_out, &aq->mutex);
    }

    int idx = (aq->queue_first + aq->queue_n) % ASYNC_QUEUE_SIZE;
    aq->queue[idx] = task;
    aq->queue_n++;

    pthread_cond_signal(&aq->queue_cond_in);
    pthread_mutex_unlock(&aq->mutex);
}

bool
GOMP_OFFLOAD_openacc_async_copy(int device, void *dst, const void *src, size_t n, struct goacc_asyncqueue *aq, bool host_to_device)
{
    if (!aq) {
        GOMP_PLUGIN_error("Invalid async queue.\n");
        return false;
    }
    struct copy_data *data = (struct copy_data *)malloc(sizeof(struct copy_data));
    data->device = device;
    data->dst = dst;
    data->src = src;
    data->len = n;
    data->aq = aq;
    data->host_to_device = host_to_device;

    queue_task task;
    task.function = data_copy_function;
    task.data = data;

    push_async_task(aq, task);
    return true;
}


bool
GOMP_OFFLOAD_openacc_async_host2dev(int device, void *dst, const void *src, size_t n, struct goacc_asyncqueue *aq)
{
    return GOMP_OFFLOAD_openacc_async_copy(device, dst, src, n, aq, true);
}


bool
GOMP_OFFLOAD_openacc_async_dev2host(int device, void *dst, const void *src, size_t n, struct goacc_asyncqueue *aq)
{
    return GOMP_OFFLOAD_openacc_async_copy(device, dst, src, n, aq, false);
}


void
execute_queue_task(struct goacc_asyncqueue *aq, int queue_index)
{
    queue_task *task = &aq->queue[queue_index];
    if (task->function) {
        task->function(task->data);
    } else {
        GOMP_PLUGIN_error("Error: Task function is NULL at index %d\n", queue_index);
    }
}

static void *
drain_queue(void *thread_arg)
{
    struct goacc_asyncqueue *aq = (struct goacc_asyncqueue *) thread_arg;

    if (aq->drain_queue_stop) {
        aq->drain_queue_stop = 2;  // here i set a finalized state
        return NULL;
    }

    pthread_mutex_lock(&aq->mutex);
    while (true) {
        if (aq->drain_queue_stop) {
            break;
        }

        if (aq->queue_n > 0) {
            pthread_mutex_unlock(&aq->mutex);  // Unlock while executing to allow other operations
            execute_queue_task(aq, aq->queue_first);

            pthread_mutex_lock(&aq->mutex);
            aq->queue_first = (aq->queue_first + 1) % ASYNC_QUEUE_SIZE;
            aq->queue_n--;

            pthread_cond_broadcast(&aq->queue_cond_out);  // Notify others waiting for queue space
            pthread_mutex_unlock(&aq->mutex);

            pthread_mutex_lock(&aq->mutex);
        } else {
            pthread_cond_wait(&aq->queue_cond_in, &aq->mutex);  // waiting for new tasks
        }
    }

    aq->drain_queue_stop = 2;
    pthread_cond_broadcast(&aq->queue_cond_out);  // here we have final broadcast to release any waiting threads
    pthread_mutex_unlock(&aq->mutex);

    return NULL;
}

struct goacc_asyncqueue *
GOMP_OFFLOAD_openacc_async_construct(int n)
{
    simulated_device *device = &devices[n];
    if (!device || !device->initialized) {
        GOMP_PLUGIN_error("Device not initialized or available.\n");
        return NULL;
    }

    pthread_mutex_lock(&device->lock);

    struct goacc_asyncqueue *aq = malloc(sizeof(*aq));
    if (!aq) {
        GOMP_PLUGIN_error("Invalid async queue.\n");
        pthread_mutex_unlock(&device->lock);
        return NULL;
    }

    aq->device = device;
    aq->prev = NULL;
    aq->next = device->async_queues;
    if (aq->next) {
        aq->next->prev = aq;
        aq->id = aq->next->id + 1;
    } else {
        aq->id = 1;
    }
    device->async_queues = aq;

    pthread_mutex_init(&aq->mutex, NULL);
    pthread_cond_init(&aq->queue_cond_in, NULL);
    pthread_cond_init(&aq->queue_cond_out, NULL);

    aq->queue_first = 0;
    aq->queue_n = 0;
    aq->drain_queue_stop = 0;

    int err = pthread_create(&aq->thread_drain_queue, NULL, drain_queue, aq);
    if (err != 0) {
        fprintf(stderr, "Failed to create asynchronous thread: %s\n", strerror(err));
        pthread_mutex_unlock(&device->lock);
        free(aq);
        return NULL;
    }

    pthread_mutex_unlock(&device->lock);
    return aq;
}


static void
finalize_async_thread(struct goacc_asyncqueue *aq)
{
    pthread_mutex_lock(&aq->mutex);
    if (aq->drain_queue_stop == 2) {
        pthread_mutex_unlock(&aq->mutex);
        return;
    }

    aq->drain_queue_stop = 1; // signal the thread to stop
    pthread_cond_signal(&aq->queue_cond_in); // wake up the thread if it's waiting

    while (aq->drain_queue_stop != 2) {
        pthread_cond_wait(&aq->queue_cond_out, &aq->mutex);
    }

    pthread_mutex_unlock(&aq->mutex);
    pthread_join(aq->thread_drain_queue, NULL);
}


bool
GOMP_OFFLOAD_openacc_async_destruct(struct goacc_asyncqueue *aq)
{
    if (!aq) {
        GOMP_PLUGIN_error("Invalid async queue.\n");
        return false;
    }
    finalize_async_thread(aq);

    pthread_mutex_lock(&aq->device->async_queues_mutex);

    // destroy mutex and condition variables
    int err;
    if ((err = pthread_mutex_destroy(&aq->mutex))) {
        fprintf(stderr, "Failed to destroy async queue mutex: %d\n", err);
        goto fail;
    }
    if (pthread_cond_destroy(&aq->queue_cond_in)) {
        fprintf(stderr, "Failed to destroy async queue condition variable\n");
        goto fail;
    }
    if (pthread_cond_destroy(&aq->queue_cond_out)) {
        fprintf(stderr, "Failed to destroy async queue condition variable\n");
        goto fail;
    }

    // update linked list of queues
    if (aq->prev) aq->prev->next = aq->next;
    if (aq->next) aq->next->prev = aq->prev;
    if (aq->device->async_queues == aq) aq->device->async_queues = aq->next;

    pthread_mutex_unlock(&aq->device->async_queues_mutex);
    free(aq);
    return true;

fail:
    pthread_mutex_unlock(&aq->device->async_queues_mutex);
    return false;
}



/// OPTIONAL
//extern int GOMP_OFFLOAD_memcpy2d (int, int, size_t, size_t,
//                                  void*, size_t, size_t, size_t,
//                                  const void*, size_t, size_t, size_t);
//extern int GOMP_OFFLOAD_memcpy3d (int, int, size_t, size_t, size_t, void *,
//                                  size_t, size_t, size_t, size_t, size_t,
//                                  const void *, size_t, size_t, size_t, size_t,
//                                  size_t);


//extern bool GOMP_OFFLOAD_can_run (void *);
//extern void GOMP_OFFLOAD_async_run (int, void *, void *, void **, void *);
//
//extern void *GOMP_OFFLOAD_openacc_create_thread_data (int);
//
//extern int GOMP_OFFLOAD_openacc_async_test (struct goacc_asyncqueue *);
//extern bool GOMP_OFFLOAD_openacc_async_synchronize (struct goacc_asyncqueue *);
//extern bool GOMP_OFFLOAD_openacc_async_serialize (struct goacc_asyncqueue *,
//                                                  struct goacc_asyncqueue *);
//extern void GOMP_OFFLOAD_openacc_async_queue_callback (struct goacc_asyncqueue *,
//                                                       void (*)(void *), void *);
//extern void *GOMP_OFFLOAD_openacc_cuda_get_current_device (void);
//extern void *GOMP_OFFLOAD_openacc_cuda_get_current_context (void);
//extern void *GOMP_OFFLOAD_openacc_cuda_get_stream (struct goacc_asyncqueue *);
//extern int GOMP_OFFLOAD_openacc_cuda_set_stream (struct goacc_asyncqueue *,
//                                                 void *);
//extern union goacc_property_value
//GOMP_OFFLOAD_openacc_get_property (int, enum goacc_property);
