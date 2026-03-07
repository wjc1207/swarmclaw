# Camera Capture

Capture an image from the network camera using http_request.
Camera URL default: `http://192.168.3.40/capture` — update if your camera has a different address.

## When to use
When the user asks to take a photo, capture an image, see what the camera sees, or check the camera.

## How to use
1. Use http_request to fetch the camera snapshot:
   → http_request url="http://192.168.3.40/capture" method="GET" enable_image_analysis="true"
2. The response will contain the image analysis result
3. Describe what you see in the image to the user

## Example
User: "What does the camera see?"
→ http_request url="http://192.168.3.40/capture" method="GET" enable_image_analysis="true"
→ Describe the captured image to the user
