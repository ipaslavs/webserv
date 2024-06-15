import requests
from concurrent.futures import ThreadPoolExecutor, as_completed
from random import choice
from time import sleep

# Function to send a random request
def send_request(url):
    methods = ["GET", "POST", "DELETE"]
    method = choice(methods)
    
    try:
        if method == "GET":
            response = requests.get(url, timeout=10)
        elif method == "POST":
            response = requests.post(url, data={"key": "value"}, timeout=10)
        elif method == "PUT":
            response = requests.put(url, data={"key": "value"}, timeout=10)
        elif method == "DELETE":
            response = requests.delete(url, timeout=10)
        return response.status_code, method, response.text
    except requests.exceptions.RequestException as e:
        return None, method, str(e)

# Main function to handle multiple concurrent requests
def test_multiple_connections(url, num_requests):
    with ThreadPoolExecutor(max_workers=num_requests) as executor:
        future_to_request = {executor.submit(send_request, url): i for i in range(num_requests)}
        for future in as_completed(future_to_request):
            index = future_to_request[future]
            try:
                status_code, method, content = future.result()
                if status_code is not None:
                    print(f"Request {index}: Method: {method} Status Code: {status_code}")
                else:
                    print(f"Request {index}: Method: {method} failed with error: {content}")
            except Exception as exc:
                print(f"Request {index} generated an exception: {exc}")

# Test parameters
url = "http://localhost:8080"  # Replace with your server's URL
num_requests = 100  # Number of concurrent connections

# Run the test
test_multiple_connections(url, num_requests)
