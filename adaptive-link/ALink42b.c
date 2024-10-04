#include <stdio.h>              // For printf, perror
#include <stdlib.h>             // For malloc, free, atoi
#include <string.h>             // For strtok, strdup
#include <unistd.h>             // For usleep
#include <pthread.h>            // For pthread functions
#include <sys/socket.h>         // For socket functions
#include <netinet/in.h>         // For sockaddr_in
#include <arpa/inet.h>          // For inet_pton
#include <stdbool.h>            // For bool, true, false
#include <sys/time.h>           // For timeval, settimeofday
#include <sys/wait.h>           // For waitpid
#include <time.h>               // For timespec, clock_gettime
#include <math.h>

#define BUFFER_SIZE 1024
#define DEFAULT_PORT 5000
#define CONFIG_FILE "/etc/txprofiles.conf"
#define MAX_PROFILES 6
#define DEFAULT_PACE_EXEC_MS 25

typedef struct {
    int rangeMin;
    int rangeMax;
    char setGI[10];
    int setMCS;
    int setFecK;
    int setFecN;
    int setBitrate;
    float setGop;
    int wfbPower;
    char ROIqp[20];
} Profile;

Profile profiles[MAX_PROFILES];
long pace_exec = DEFAULT_PACE_EXEC_MS * 1000L;
int currentProfile = -1;
int previousProfile = -2;
long prevTimeStamp = 0;
bool verbose_mode = false;
int message_count = 0;      // Global variable for message count
bool paused = false;        // Global variable for pause state
bool time_synced = false;   // Global flag to indicate if time has been synced
int last_value_sent = -1;   // Track the last value sent to channels.sh
struct timespec last_exec_time; // Last execution time of channels.sh
struct timespec last_keyframe_request_time;	// Last keyframe request command
bool script_running = false; // Global flag for tracking if channels.sh is running
pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;  // Mutex for message count
pthread_mutex_t pause_mutex = PTHREAD_MUTEX_INITIALIZER;  // Mutex for pause state

long get_monotonic_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

// Function to load profiles from config file
void load_profiles(const char* filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error opening config file");
        exit(1);
    }

    int i = 0;
    while (fscanf(file, "%d - %d %s %d %d %d %d %f %d %s",
                  &profiles[i].rangeMin, &profiles[i].rangeMax, profiles[i].setGI,
                  &profiles[i].setMCS, &profiles[i].setFecK, &profiles[i].setFecN,
                  &profiles[i].setBitrate, &profiles[i].setGop, &profiles[i].wfbPower,
                  profiles[i].ROIqp) != EOF && i < MAX_PROFILES) {
        i++;
    }

    fclose(file);
}


// Get the profile based on input value
Profile* get_profile(int input_value) {
    for (int i = 0; i < MAX_PROFILES; i++) {
        if (input_value >= profiles[i].rangeMin && input_value <= profiles[i].rangeMax) {
            return &profiles[i];
        }
    }
    return NULL;
}

// Execute system command
void execute_command(const char* command) {
    system(command);
}

// Command execution logic
void apply_profile(Profile* profile) {
    // Construct commands
    char powerCommand[100];
    char mcsCommand[100];
    char bitrateCommand[150];
    char gopCommand[100];
    char fecCommand[100];
    char roiCommand[150];
	char msposdCommand[300];

    sprintf(powerCommand, "iw dev wlan0 set txpower fixed %d", profile->wfbPower * 50);
    sprintf(mcsCommand, "wfb_tx_cmd 8000 set_radio -B 20 -G %s -S 1 -L 1 -M %d", profile->setGI, profile->setMCS);
    sprintf(bitrateCommand, "curl -s 'http://localhost/api/v1/set?video0.bitrate=%d'", profile->setBitrate);
    sprintf(gopCommand, "curl -s 'http://localhost/api/v1/set?video0.gopSize=%f'", profile->setGop);
    sprintf(fecCommand, "wfb_tx_cmd 8000 set_fec -k %d -n %d", profile->setFecK, profile->setFecN);
    sprintf(roiCommand, "curl -s 'http://localhost/api/v1/set?fpv.roiQp=%s'", profile->ROIqp);

	// Calculate seconds since last change
    long currentTime = get_monotonic_time();
    long timeElapsed = currentTime - prevTimeStamp; // Time since the last change

    // Logic to determine execution order
    if (currentProfile > previousProfile) {
        execute_command(powerCommand); usleep(10000);
        execute_command(gopCommand); usleep(10000);
        execute_command(mcsCommand); usleep(75000);
        execute_command(fecCommand); usleep(75000);
        execute_command(bitrateCommand); 
        execute_command(roiCommand); usleep(75000);
    } else {
        execute_command(bitrateCommand); 
        execute_command(gopCommand); usleep(75000);
        execute_command(mcsCommand); usleep(75000);
        execute_command(fecCommand); usleep(75000);
        execute_command(powerCommand); usleep(75000);
        execute_command(roiCommand); usleep(75000);
    }
	
	// Display stats on msposd
    sprintf(msposdCommand, "echo \"%ld s %d M:%d %s F:%d/%d P:%d G:%.1f&L30&F28 CPU:&C &Tc\" >/tmp/MSPOSD.msg",
            timeElapsed, profile->setBitrate, profile->setMCS, profile->setGI,
            profile->setFecK, profile->setFecN, profile->wfbPower, profile->setGop);
    execute_command(msposdCommand); // Execute the msposd command
	
}

void value_chooses_profile(int input_value) {
    // Get the appropriate profile based on input
    Profile* selectedProfile = get_profile(input_value);
    if (selectedProfile == NULL) {
        printf("No matching profile found for input: %d\n", input_value);
		return;
    }

    // Find the index of the selected profile
    
    for (int i = 0; i < MAX_PROFILES; i++) {
        if (selectedProfile == &profiles[i]) {
            currentProfile = i;
            break;
        }
    }

    // If the previous profile is the same, do not apply changes
    if (previousProfile == currentProfile) {
        printf("No changes required. Profile is unchanged.\n");
		return;
    }

    // Check if a change is needed based on time constraints
    long currentTime = get_monotonic_time();
    long timeElapsed = currentTime - prevTimeStamp;

    if (previousProfile == 0 && timeElapsed <= 3) {
        return;
    }
    if ((currentProfile - previousProfile == 1) && timeElapsed <= 2) {
        return;
    }

    // Apply the selected profile
    apply_profile(selectedProfile);
	// Update previousProfile 
    previousProfile = currentProfile;
	prevTimeStamp = currentTime;
}

void start_selection(int rssi_score, int snr_score) {
	int value;
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);  // Use CLOCK_MONOTONIC

    long time_diff_ms = (current_time.tv_sec - last_exec_time.tv_sec) * 1000 +
                    (current_time.tv_nsec - last_exec_time.tv_nsec) / 1000000;


	if (rssi_score != 999) {	
		
		// Combine rssi and snr by weight
		float w_rssi = 0.4;
		float w_snr = 0.6;
		int combined_value = floor(rssi_score * w_rssi + snr_score * w_snr); //combine and round down
		
		// Check if combined_value exceeds
		if (combined_value < 1000) {
			combined_value = 1000;  // Set to 1000 if too low
		}
		if (combined_value > 2000) {
			combined_value = 2000;  // Set to 2000 if it exceeds
		}
		
		value = combined_value;
				
		// Apply hysteresis here if required
				
	
	} else {
		value = 999;
	}

    if ((value != last_value_sent && time_diff_ms >= 100) || (value == last_value_sent && time_diff_ms >= 2000)) {
        
		value_chooses_profile(value);
        printf("Requesting profile for: %d\n", value);
        
		// Update the last sent value and time
        last_value_sent = value;
        last_exec_time = current_time;
    } else {
        printf("Skipping profile load: value=%d, time_diff_ms=%ldms",
               value, time_diff_ms);
    }

	
}



void special_command_message(const char *msg) {
    // Move past "special:"
    const char *cleaned_msg = msg + 8;

    // Find the next ':'
    char *separator = strchr(cleaned_msg, ':');
    if (separator) {
        // Truncate the string at the first ':'
        *separator = '\0';
    }

    // Process the cleaned message
    if (strcmp(cleaned_msg, "pause_adaptive") == 0) {
        pthread_mutex_lock(&pause_mutex);
        paused = true;
        pthread_mutex_unlock(&pause_mutex);
        printf("Paused adaptive mode\n");
    } else if (strcmp(cleaned_msg, "resume_adaptive") == 0) {
        pthread_mutex_lock(&pause_mutex);
        paused = false;
        pthread_mutex_unlock(&pause_mutex);
        printf("Resumed adaptive mode\n");
	//} else if (strcmp(cleaned_msg, "drop_gop") == 0) {
    //    printf("Dropping GOP due to lost packets\n");
	//	system("curl localhost/api/v1/set?video0.gopSize=0.01 &");
	//	execute_channels_script(998, 1000);
    } else if (strcmp(cleaned_msg, "request_keyframe") == 0) {
        struct timespec current_time;
		clock_gettime(CLOCK_MONOTONIC, &current_time);  // Use CLOCK_MONOTONIC
		long time_diff_ms = (current_time.tv_sec - last_keyframe_request_time.tv_sec) * 1000 +
        (current_time.tv_nsec - last_keyframe_request_time.tv_nsec) / 1000000;

        if (time_diff_ms >= 100) {  // Check if at least 100ms has passed
            printf("Requesting new keyframe\n");
            system("echo IDR 0 | nc localhost 4000");  // Request new keyframe

            // Update the last keyframe request time
            last_keyframe_request_time = current_time;
        } else {
            printf("Skipping keyframe request: time_diff_ms=%ldms\n", time_diff_ms);
        }
    } else {
        // Handle unknown commands if needed
        printf("Unknown command: %s\n", cleaned_msg);
    }
}

void *count_messages(void *arg) {
    int local_count;
    while (1) {
        usleep(500000);  // Sleep for 500ms
        pthread_mutex_lock(&count_mutex);
        local_count = message_count;
        message_count = 0;  // Reset the count every 500ms
        pthread_mutex_unlock(&count_mutex);

        pthread_mutex_lock(&pause_mutex);
        if (local_count == 0 && !paused) {
            printf("No messages received in 500ms, sending 999\n");
            start_selection(999, 1000);
			//execute_channels_script(999, 1000);
        } else {
            printf("Messages per 500ms: %d\n", local_count);
        }
        pthread_mutex_unlock(&pause_mutex);
    }
    return NULL;
}

void process_message(const char *msg) {
    		
	// Declare local variables
    struct timeval tv;
    int transmitted_time = 0;
    int link_value_rssi = 999;
    int link_value_snr = 999;
	int recovered = 0;
    int lost = 0;
    int rssi1 = -105;
    int rssi2 = -105;
    int rssi3 = -105;
    int rssi4 = -105;

    // Copy the input string to avoid modifying the original
    char *msgCopy = strdup(msg);
    if (msgCopy == NULL) {
        perror("Failed to allocate memory");
        return;
    }

    // Use strtok to split the string by ':'
    char *token = strtok(msgCopy, ":");
    int index = 0;

    // Iterate through tokens and convert to integers
    while (token != NULL) {
        switch (index) {
            case 0:
                transmitted_time = atoi(token);
                break;
            case 1:
                link_value_rssi = atoi(token);
				break;
			case 2:
                link_value_snr = atoi(token);
                break;
            case 3:
                recovered = atoi(token);
                break;
            case 4:
                lost = atoi(token);
                break;
            case 5:
                rssi1 = atoi(token);
                break;
            case 6:
                rssi2 = atoi(token);
                break;
            case 7:
                rssi3 = atoi(token);
                break;
            case 8:
                rssi4 = atoi(token);
                break;
            default:
                // Ignore extra tokens
                break;
        }
        token = strtok(NULL, ":");
        index++;
    }

    // Print parsed values (for demonstration purposes)
   // printf("Parsed values: %d, %d, %d, %d, %d, %d, %d, %d, %d\n",
     //      transmitted_time, link_value_rssi, link_value_snr, recovered, lost, rssi1, rssi2, rssi3, rssi4);

    // Free the duplicated string
    free(msgCopy);
	
	// Only proceed with time synchronization if it hasn't been set yet
    if (!time_synced) {
        if (transmitted_time > 0) {
            tv.tv_sec = transmitted_time;
            tv.tv_usec = 0;
            if (settimeofday(&tv, NULL) == 0) {
                printf("System time synchronized with transmitted time: %ld\n", (long)transmitted_time);
                time_synced = true;
            } else {
                perror("Failed to set system time");
            }
        }
	}

    // Handle the adaptive mode pause state
    pthread_mutex_lock(&pause_mutex);
    if (!paused) {
        start_selection(link_value_rssi, link_value_snr);
		//execute_channels_script(link_value_rssi, link_value_snr);
    } else {
        printf("Adaptive mode paused, skipping execution of channels.sh\n");
    }
        pthread_mutex_unlock(&pause_mutex);

}


void print_usage() {
    printf("Usage: ./udp_server --port <port> --pace-exec <time> --verbose\n");
    printf("Options:\n");
    printf("  --port       Port to listen on (default: %d)\n", DEFAULT_PORT);
    printf("  --verbose    Enable verbose output\n");
    printf("  --pace-exec  Maj/wfb control execution pacing interval in milliseconds (default: %d ms)\n", DEFAULT_PACE_EXEC_MS);
}

int main(int argc, char *argv[]) {
    
	// Load profiles from configuration file
    load_profiles(CONFIG_FILE);
		
	int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];
    int port = DEFAULT_PORT;

    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose_mode = true;
        } else if (strcmp(argv[i], "--pace-exec") == 0 && i + 1 < argc) {
            int ms = atoi(argv[++i]);
            pace_exec = ms * 1000L; // Convert milliseconds to microseconds
        } else {
            print_usage();
            return 1;
        }
    }

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Initialize server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(port);

    // Bind the socket
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    if (verbose_mode) {
        printf("Listening on UDP port %d...\n", port);
    }

    // Prepare counting thread
	pthread_t count_thread;
    pthread_create(&count_thread, NULL, count_messages, NULL);


 while (1) {
        // Receive a message
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                         (struct sockaddr *)&client_addr, &client_addr_len);
        if (n < 0) {
            perror("recvfrom failed");
            break;
        }
		
		// Increment message count
			pthread_mutex_lock(&count_mutex);
			message_count++;
			pthread_mutex_unlock(&count_mutex);

        // Null-terminate the received data
        buffer[n] = '\0';

        // Extract the length of the message (first 4 bytes)
        uint32_t msg_length;
        memcpy(&msg_length, buffer, sizeof(msg_length));
        msg_length = ntohl(msg_length); // Convert from network to host byte order

        // Print the message length and content
        if (verbose_mode) {
			printf("Received message (%u bytes): %s\n", msg_length, buffer + sizeof(msg_length));
					
		}
		
		// Strip length off the start of the message
		char *message = buffer + sizeof(uint32_t);
		//See if it's special otherwise just process it
		if (strncmp(message, "special:", 8) == 0) {
			special_command_message(message);
		} else {
			process_message(message);
		}	
		
    }

    // Close the socket
    close(sockfd);
    return 0;
}
