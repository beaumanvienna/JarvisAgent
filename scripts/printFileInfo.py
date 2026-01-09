import os
import datetime
from pathlib import Path

def get_file_info(filename):
    try:
        # Create a Path object
        file_path = Path(filename)
        
        # Check if the file exists
        if not file_path.is_file():
            print(f"Error: The file '{filename}' does not exist.")
            return

        # Get file statistics
        stats = file_path.stat()
        
        # Convert size to a readable format (Bytes)
        size = stats.st_size
        
        # Get creation time 
        # Note: On Unix/Linux, st_ctime is the time of the last metadata change. 
        # On Windows, it is the actual creation time.
        creation_time = datetime.datetime.fromtimestamp(stats.st_ctime)

        print(f"--- File Information: {filename} ---")
        print(f"Size: {size} bytes")
        print(f"Created on: {creation_time.strftime('%Y-%m-%d %H:%M:%S')}")
        
    except Exception as e:
        print(f"An error occurred: {e}")

if __name__ == "__main__":
    user_input = input("Enter the filename (with path if necessary): ")
    get_file_info(user_input)
