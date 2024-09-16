#!/bin/bash

SERVER_URL="http://localhost:8080"
UPLOAD_ENDPOINT="/upload_handler"
UPLOAD_DIR="./web/uploads/example1/upload_test"
TEST_FILE="test_file.txt"

# Create a test file locally
echo "Test content for DELETE method." > $TEST_FILE

# Upload the file to the server
curl -X POST -F "uploaded_file=@$TEST_FILE" "$SERVER_URL$UPLOAD_ENDPOINT"

# Confirm file exists on the server
if [ -f "$UPLOAD_DIR/$TEST_FILE" ]; then
    echo "File uploaded successfully."
else
    echo "File upload failed."
    exit 1
fi

# Send DELETE request
curl -v -X DELETE "$SERVER_URL$UPLOAD_ENDPOINT/$TEST_FILE"

# Confirm file has been deleted
if [ ! -f "$UPLOAD_DIR/$TEST_FILE" ]; then
    echo "File deleted successfully."
else
    echo "File deletion failed."
fi

# Clean up local test file
rm -f $TEST_FILE
