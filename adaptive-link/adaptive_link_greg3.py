#!/usr/bin/python3
import socket
import msgpack
import struct
import threading
import time
import argparse
import os
import configparser

VERSION = "0.1.1"

# Global variables
results = []
link_health_score_rssi = 1000  # Default score before calculation for RSSI
link_health_score_snr = 1000  # Default score before calculation for SNR
recovered_packets = 0  # To store the number of recovered packets (FEC)
lost_packets = 0  # To store the number of lost packets
best_antennas_rssi = [-105, -105, -105, -105]  # Default values for best antennas RSSI if less than 4
best_antennas_snr = [-105, -105, -105, -105]  # Default values for best antennas SNR if less than 4
config = None  # Configuration file object
udp_socket = None  # To store the UDP socket globally
udp_ip = None  # To store the UDP IP globally
udp_port = None  # To store the UDP port globally
previous_link_health_score_rssi = None
previous_link_health_score_snr = None



# Default config values
DEFAULT_CONFIG = {
    'Settings': {
        'version': VERSION,  # Current version of the script
        'message_interval': '100',  # Time between sending messages (milliseconds)
        'use_best_rssi': 'True',  # Option to pick best available RSSI for link health score
        'min_rssi': '-105',  # Min RSSI range for link health
        'max_rssi': '-60',  # Max RSSI range for link health
        'min_snr': '12',  # Min SNR range for link health
        'max_snr': '28',  # Max SNR range for link health
        'host': '127.0.0.1',  # Host address to connect to
        'port': '8003',  # Port to connect to
        'udp_ip': '10.5.0.10',  # UDP IP to send link health data
        'udp_port': '9999',  # UDP port to send link health data
        'retry_interval': '1',  # Time in seconds to wait before retrying TCP connection on failure
    },
    'Descriptions': {
        'version': 'Version of the script',
        'message_interval': 'Time between sending UDP messages in milliseconds',
        'use_best_rssi': 'Use True to pick the best available RSSI for health score, or False for average RSSI',
        'min_rssi': 'Minimum RSSI value used for link health calculation',
        'max_rssi': 'Maximum RSSI value used for link health calculation',
        'min_snr': 'Minimum SNR value used for link health calculation',
        'max_snr': 'Maximum SNR value used for link health calculation',
        'host': 'Host address to connect to for the TCP connection',
        'port': 'Port to connect to for the TCP connection',
        'udp_ip': 'UDP IP to send the link health data',
        'udp_port': 'UDP port to send the link health data',
        'retry_interval': 'Time in seconds to wait before retrying TCP connection on failure'
    }
}

def load_config(config_file='config.ini'):
    global config
    config = configparser.ConfigParser()

    # Check if config file exists, otherwise create it with default values
    if not os.path.exists(config_file):
        print(f"Config file {config_file} not found. Creating with default values...")
        config.read_dict(DEFAULT_CONFIG)
        with open(config_file, 'w') as f:
            config.write(f)
    else:
        config.read(config_file)

    # Ensure the version is updated in the config file
    if config['Settings'].get('version') != VERSION:
        print(f"Updating version in config file from {config['Settings'].get('version')} to {VERSION}")
        config['Settings']['version'] = VERSION
        update_version_history(config_file)

    # Ensure config file is up to date (i.e., if a new field is added)
    updated = False
    for section in DEFAULT_CONFIG:
        if section not in config:
            config[section] = DEFAULT_CONFIG[section]
            updated = True
        else:
            for key, value in DEFAULT_CONFIG[section].items():
                if key not in config[section]:
                    config[section][key] = value
                    updated = True

    if updated:
        with open(config_file, 'w') as f:
            config.write(f)

def update_version_history(config_file):
    """
    Updates the version history in the configuration file.
    """
    if 'Version History' not in config:
        config['Version History'] = {}

    # Add the current version with a timestamp
    timestamp = time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime())
    config['Version History'][VERSION] = f"Version {VERSION} updated on {timestamp}"

    # Write the updated config to file
    with open(config_file, 'w') as f:
        config.write(f)

def request_keyframe():
    """
    Send a special message to request a keyframe
    """
    special_message = "special:request_keyframe"
    num_attempts = 5  # Number of times to send the message

    for attempt in range(num_attempts):
        send_udp(special_message)  # Call send_udp to send the special message
        if verbose_mode:
            print(f"Sent special message: {special_message}, attempt {attempt + 1}/{num_attempts}")
        time.sleep(0.2)  # Wait before the next attempt
    
def drop_gop():
    """
    Send a special message to drop the gop
    """
    special_message = "special:drop_gop"
    num_attempts = 5  # Number of times to send the message

    for attempt in range(num_attempts):
        send_udp(special_message)  # Call send_udp to send the special message
        if verbose_mode:
            print(f"Sent special message: {special_message}, attempt {attempt + 1}/{num_attempts}")
        time.sleep(0.2)  # Wait before the next attempt


def calculate_link_health(video_rx):
    global recovered_packets, lost_packets, link_health_score_rssi, link_health_score_snr, best_antennas_rssi, best_antennas_snr
    global previous_link_health_score_rssi, previous_link_health_score_snr


    try:
        # Get configuration settings
        message_interval = int(config['Settings']['message_interval'])
        use_best_rssi = config.getboolean('Settings', 'use_best_rssi')
        min_rssi = float(config['Settings']['min_rssi'])
        max_rssi = float(config['Settings']['max_rssi'])
        min_snr = float(config['Settings']['min_snr'])
        max_snr = float(config['Settings']['max_snr'])

        # Get packets data
        packets = video_rx.get('packets', {})  # Access the 'packets' field

        # Get FEC recovery stats from the 'packets' field
        fec_rec = packets.get('fec_rec', [0, 0])
        recovered_packets = fec_rec[0]  # Update the global recovered_packets
        
        # Get lost packet stats from the 'packets' field
        lost = packets.get('lost', [0, 0])
        lost_packets = lost[0]  # Update the global lost_packets
        
        # Check lost_packets and send special message if condition is met
        #if 0 < lost_packets < 10:
        #    request_keyframe()  # Send request_keyframe message
            #drop_gop() # Send drop_gop message
           
        
        # Get antenna stats
        rx_ant_stats = video_rx.get('rx_ant_stats', {})
        rssi_values = []
        snr_values = []
        num_antennas = len(rx_ant_stats)

        # Ensure there are antenna stats available
        if num_antennas > 0:
            for antenna in rx_ant_stats.values():  # Antenna data is stored in a dict, so use .values()
                if len(antenna) >= 6:  # Check if antenna data has enough values to access rssi_avg and snr_avg
                    rssi_avg = antenna[2]  # rssi_avg is at index 2
                    snr_avg = antenna[5]  # snr_avg is at index 5 (assuming it's the middle of min, avg, max)
                    rssi_values.append(rssi_avg)
                    snr_values.append(snr_avg)
                else:
                    if verbose_mode:
                        print(f"Warning: Antenna data is incomplete: {antenna}")
        else:
            if verbose_mode:
                print("Warning: No antenna stats available")
            link_health_score_rssi = 1000  # Default to 1000 when no antenna stats are available
            link_health_score_snr = 1000  # Optionally set default SNR as well if needed
            best_antennas_rssi = [-105, -105, -105, -105]  # Set default RSSI values for missing antennas
            best_antennas_snr = [-105, -105, -105, -105]  # Set default SNR values for missing antennas
            return link_health_score_rssi, link_health_score_snr  # Exit early if no antennas

        # Sort RSSI and SNR values to get the best 4 antennas
        rssi_values.sort(reverse=True)
        snr_values.sort(reverse=True)

        best_antennas_rssi = rssi_values[:4]  # Get the top 4 antennas' RSSI values
        best_antennas_rssi += [-105] * (4 - len(best_antennas_rssi))  # If fewer than 4 antennas, pad with -105

        best_antennas_snr = snr_values[:4]  # Get the top 4 antennas' SNR values
        best_antennas_snr += [-105] * (4 - len(best_antennas_snr))  # If fewer than 4 antennas, pad with -105

        # Calculate link health score for RSSI
        if use_best_rssi:
            rssi_to_use = best_antennas_rssi[0]  # Use the best RSSI
        else:
            rssi_to_use = sum(best_antennas_rssi) / 4  # Use the average of the best 4 RSSI values

        if rssi_to_use > max_rssi:
            link_health_score_rssi = 2000  # Max health when RSSI is better than max_rssi
        elif rssi_to_use < min_rssi:
            link_health_score_rssi = 1000  # Min health when RSSI is worse than min_rssi
        else:
            # Linear interpolation between min_rssi and max_rssi mapped to 1000 and 2000
            link_health_score_rssi = 1000 + ((rssi_to_use - min_rssi) / (max_rssi - min_rssi)) * 1000

        # Calculate link health score for SNR
        avg_best_snr = sum(best_antennas_snr) / 4
        if avg_best_snr > max_snr:
            link_health_score_snr = 2000  # Max health when SNR is better than max_snr
        elif avg_best_snr < min_snr:
            link_health_score_snr = 1000  # Min health when SNR is worse than min_snr
        else:
            # Linear interpolation between min_snr and max_snr mapped to 1000 and 2000
            link_health_score_snr = 1000 + ((avg_best_snr - min_snr) / (max_snr - min_snr)) * 1000
        
        # Implement hysteresis logic
        #rssi_change = abs(link_health_score_rssi - previous_link_health_score_rssi) / previous_link_health_score_rssi if previous_link_health_score_rssi else 1
        #snr_change = abs(link_health_score_snr - previous_link_health_score_snr) / previous_link_health_score_snr if previous_link_health_score_snr else 1

        #if (rssi_change > 0.15) or (link_health_score_rssi > previous_link_health_score_rssi):
        #    previous_link_health_score_rssi = link_health_score_rssi

        #if (snr_change > 0.15) or (link_health_score_snr > previous_link_health_score_snr):
        #    previous_link_health_score_snr = link_health_score_snr
        
        
        # Round the health scores to the nearest integer
        link_health_score_rssi = round(link_health_score_rssi)
        link_health_score_snr = round(link_health_score_snr)

        
        if verbose_mode:
            print(f"Calculated Health Score RSSI: {link_health_score_rssi}, SNR: {link_health_score_snr}, Recovered: {recovered_packets}, Lost: {lost_packets}, Best Antennas RSSI: {best_antennas_rssi}, Best Antennas SNR: {best_antennas_snr}")
        
        return link_health_score_rssi, link_health_score_snr
    
    except Exception as e:
        if verbose_mode:
            print(f"Error calculating link health: {e}, video_rx data: {video_rx}")
        return 1000, 1000  # Return worst-case score if something goes wrong

def send_udp(message):
    """
    Adds message length to the start and sends message on provided port
    """
    if verbose_mode:
        print("send_udp function has started")  # Debug statement to confirm function start
            
    # Prepend the size of the message as a 4-byte unsigned integer
    message_bytes = message.encode('utf-8')
    message_size = struct.pack('!I', len(message_bytes))  # Use network byte order (big-endian)

    # Full message with size prefix
    full_message = message_size + message_bytes
    
    if verbose_mode:
        print("Preparing UDP message to be sent")  # Debug statement
    
    # Send the message
    try:
        udp_socket.sendto(full_message, (udp_ip, udp_port))
        if verbose_mode:
            print(f"UDP Message Sent: {message} (size: {len(message_bytes)} bytes)")
    except Exception as e:
        if verbose_mode:
            print(f"Error sending UDP data: {e}")

def generate_package():
    """
    Generate, at interval, string with all the variables
    """
    message_interval = int(config['Settings']['message_interval']) / 1000  # Convert to seconds
    
    while True:
        timestamp = int(time.time())  # Get epoch time in seconds since 1970
        # Include best antennas in the message
        message = f"{timestamp}:{link_health_score_rssi}:{link_health_score_snr}:{recovered_packets}:{lost_packets}:{best_antennas_rssi[0]}:{best_antennas_rssi[1]}:{best_antennas_rssi[2]}:{best_antennas_rssi[3]}"
                    
        # Pass the message to send_udp function
        send_udp(message)
                
        time.sleep(message_interval)  # Send at the specified interval


def connect_and_receive_msgpack():
    global results, link_health_score_rssi, link_health_score_snr, recovered_packets, lost_packets, best_antennas_rssi, best_antennas_snr
    global udp_socket, udp_ip, udp_port  # Make the UDP socket, IP, and port available globally
    udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # Create UDP socket

    # Get host, port, and UDP destination from the config
    host = config['Settings']['host']
    port = int(config['Settings']['port'])
    udp_ip = config['Settings']['udp_ip']
    udp_port = int(config['Settings']['udp_port'])
    retry_interval = int(config['Settings']['retry_interval'])  # Get retry interval from config

    while True:
        try:
            # Create a TCP/IP socket for receiving data
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client_socket:
                # Connect to the remote server
                if verbose_mode:
                    print(f"Connecting to {host}:{port}...")
                client_socket.connect((host, port))
                if verbose_mode:
                    print(f"Connected to {host}:{port}")

                # Start UDP sending in a separate thread
                udp_thread = threading.Thread(target=generate_package, daemon=True)  # Change here to call generate_package
                udp_thread.start()

                while True:
                    # First, read 4 bytes for the length prefix
                    length_prefix = client_socket.recv(4)
                    if not length_prefix:
                        if verbose_mode:
                            print("No more data, connection closed.")
                        break

                    # Unpack the 4-byte length prefix (uint32)
                    msg_length = struct.unpack('!I', length_prefix)[0]

                    # Now read the actual MessagePack data of the given length
                    data = b""
                    while len(data) < msg_length:
                        chunk = client_socket.recv(min(4096, msg_length - len(data)))
                        if not chunk:
                            if verbose_mode:
                                print("Incomplete data, connection closed.")
                            break
                        data += chunk

                    # If we successfully received the full message, unpack it
                    if len(data) == msg_length:
                        try:
                            unpacked_data = msgpack.unpackb(data, use_list=False, strict_map_key=False)
                            results.append(unpacked_data)  # Append to the results array

                            # Process video_rx message
                            if unpacked_data.get("type") == "rx" and unpacked_data.get("id") == "video rx":
                                link_health_score_rssi, link_health_score_snr = calculate_link_health(unpacked_data)
                                if verbose_mode:
                                    print(f"Link Health Score RSSI: {link_health_score_rssi}, SNR: {link_health_score_snr}, Best Antennas RSSI: {best_antennas_rssi}, Best Antennas SNR: {best_antennas_snr}")
                                
                        except msgpack.UnpackException as e:
                            if verbose_mode:
                                print(f"Failed to unpack data: {e}")
                    else:
                        if verbose_mode:
                            print("Failed to receive full data, closing connection.")
                        break
        except Exception as e:
            if verbose_mode:
                print(f"Connection failed or lost: {e}. Retrying in {retry_interval} seconds...")
            time.sleep(retry_interval)  # Wait before retrying connection

if __name__ == "__main__":
    # Set up argument parser
    parser = argparse.ArgumentParser(description="TCP MessagePack client with UDP link health reporting and configuration.")
    parser.add_argument('--config', type=str, help='Path to configuration file', default='config.ini')  # New config argument
    parser.add_argument('--verbose', action='store_true', help='Enable verbose mode for logging')  # Verbose argument added
    
    # Parse the arguments
    args = parser.parse_args()

    # Enable verbose mode if specified
    global verbose_mode
    verbose_mode = args.verbose

    # Load the config from the specified location
    load_config(args.config)

    # Connect to the remote server and start receiving data
    connect_and_receive_msgpack()
