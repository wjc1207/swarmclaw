# A2A Protocol (Agent-to-Agent) Guide

## Introduction

The **A2A (Agent-to-Agent) Protocol** is an open standard that enables AI agents to discover and communicate with each other seamlessly. It allows agents to exchange capabilities, intentions, and data in a standardized way, enabling interoperability across different agent frameworks and platforms.

A2A protocol defines:
- A standardized **Agent Card** format for agent capability discovery
- A common messaging protocol for agent-to-agent communication
- Secure authentication and authorization mechanisms
- Service discovery mechanisms

Official documentation: https://a2a-protocol.org/latest/

## What is an Agent Card?

An Agent Card is a standardized JSON document that describes an agent's capabilities, endpoints, authentication requirements, and interaction protocols. It allows other agents to discover how to communicate with and utilize the services provided by this agent.

A typical Agent Card contains:
- Agent metadata (name, description, version, developer)
- Endpoint URLs for communication
- Authentication requirements
- Supported capabilities and methods
- Schema definitions for input/output
- Rate limits and usage policies

## How to Read an Agent Card

### Step 1: Retrieve the Agent Card
Agent Cards are typically exposed via well-known endpoints on the agent's server:

```python
import requests

# Fetch agent card from well-known endpoint
agent_url = "https://example-agent.com"
response = requests.get(f"{agent_url}/.well-known/agent-card.json")
agent_card = response.json()
```

### Step 2: Parse Basic Information
Extract essential metadata:

```python
agent_name = agent_card.get("name")
agent_description = agent_card.get("description")
version = agent_card.get("version")
endpoints = agent_card.get("endpoints", {})
```

### Step 3: Identify Communication Endpoint
Find the A2A messaging endpoint:

```python
# Get the A2A endpoint
a2a_endpoint = endpoints.get("a2a", {}).get("url")
transport = endpoints.get("a2a", {}).get("transport", "http")
```

### Step 4: Check Authentication Requirements
Understand what authentication is needed:

```python
auth = agent_card.get("authentication", {})
auth_type = auth.get("type")  # none, api_key, jwt, oauth2
required_scopes = auth.get("scopes", [])
```

### Step 5: Enumerate Capabilities
List what the agent can do:

```python
capabilities = agent_card.get("capabilities", [])
for capability in capabilities:
    print(f"Capability: {capability['name']} - {capability['description']}")
    print(f"Input schema: {capability.get('input_schema')}")
    print(f"Output schema: {capability.get('output_schema')}")
```

## How to Operate According to the Agent Card

Once you've read and understood the Agent Card, follow these steps to interact with the agent:

### 1. Set Up Authentication
According to the auth type specified in the Agent Card:

```python
headers = {}

if auth_type == "api_key":
    headers["X-API-Key"] = "your-api-key-here"
elif auth_type == "jwt":
    token = get_jwt_token()  # Obtain JWT token
    headers["Authorization"] = f"Bearer {token}"
```

### 2. Prepare the Request
Structure your request according to the capability's input schema:

```python
# Select the capability you want to invoke
target_capability = capabilities[0]  # Example: first capability

request_body = {
    "jsonrpc": "2.0",
    "id": "request-1",
    "method": target_capability["name"],
    "params": {
        # Fill in parameters according to input schema
        "prompt": "Hello agent!",
        "parameters": {
            # Additional parameters
        }
    }
}
```

### 3. Send the A2A Request
Send the request to the A2A endpoint:

```python
response = requests.post(
    a2a_endpoint,
    json=request_body,
    headers=headers
)

if response.status_code == 200:
    result = response.json()
    if "result" in result:
        # Handle successful response
        output = result["result"]
        print(f"Agent response: {output}")
    elif "error" in result:
        # Handle error
        error = result["error"]
        print(f"Error: {error['message']} (code: {error['code']})")
else:
    print(f"HTTP Error: {response.status_code}")
```

### 4. Follow Streaming Protocols (if supported)
If the agent supports streaming responses:

```python
if target_capability.get("streaming", False):
    response = requests.post(
        a2a_endpoint,
        json=request_body,
        headers=headers,
        stream=True
    )
    for line in response.iter_lines():
        if line:
            # Process each streaming chunk
            print(f"Chunk: {line.decode('utf-8')}")
```

## Best Practices

1. **Always validate** against the input/output schemas provided in the Agent Card
2. **Respect rate limits** specified in the Agent Card's `usage` section
3. **Handle errors gracefully** according to the error schema
4. **Cache the Agent Card** to avoid unnecessary fetching
5. **Support version negotiation** for backward compatibility

## References

- Official Website: https://a2a-protocol.org/latest/
- GitHub Repository: [Check the official site for the latest repository link]
- Specification: Available on the official website
