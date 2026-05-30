import cv2
import numpy as np
import socket

# Instructions:
# 1. Install dependencies: pip install opencv-python numpy
# 2. Run this script: python jpeg_receiver.py
# 3. Start mirroring from the Android app.

UDP_IP = "192.168.1.178" # Listen on all available network interfaces
UDP_PORT = 5000

def main():
    # Create a UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_IP, UDP_PORT))

    print(f"Listening for JPEG stream on {UDP_IP}:{UDP_PORT}...")
    print("Press 'q' in the window to quit.")

    buffer = bytearray()

    try:
        while True:
            # Receive data (max packet size is usually much smaller, but 65535 is the UDP limit)
            data, addr = sock.recvfrom(65535)
            buffer.extend(data)

            # A JPEG image starts with 0xFF 0xD8 (SOI) and ends with 0xFF 0xD9 (EOI)
            while True:
                # Find start of image
                start = buffer.find(b'\xff\xd8')
                if start == -1:
                    # No start marker found, but keep the last byte in case it's part of a marker
                    if len(buffer) > 0:
                        buffer = buffer[-1:]
                    break

                # Discard everything before the start marker
                if start > 0:
                    buffer = buffer[start:]

                # Find end of image
                end = buffer.find(b'\xff\xd9')
                if end == -1:
                    # Start found but end not yet received, wait for more data
                    break

                # Extract the full JPEG frame
                jpg_data = buffer[:end+2]
                # Remove the processed frame from the buffer
                buffer = buffer[end+2:]

                # Decode the JPEG to a BGR image for OpenCV
                nparr = np.frombuffer(jpg_data, np.uint8)
                img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)

                if img is not None:
                    # Display the image
                    cv2.imshow('Android Screen Mirror (JPEG)', img)

                # Check for 'q' key to exit
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    return

    except KeyboardInterrupt:
        print("\nStopping receiver...")
    finally:
        sock.close()
        cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
