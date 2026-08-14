#!/usr/bin/env python3
"""
ESP-Claw Tri-Mode SSH Gateway Server
Supports:
 1. MCP Streamable HTTP: http://0.0.0.0:8088/mcp — the only MCP transport ESP-Claw's
    mcp-c-sdk client speaks; use this one from the device.
 2. MCP legacy SSE Protocol: http://0.0.0.0:8088/sse — for desktop clients (Claude Desktop,
    Cursor, ...). The device cannot use it: the C SDK never implemented the 2024-11-05
    "GET /sse + event: endpoint" handshake.
 3. Standard HTTP REST Endpoint (fallback that needs no MCP client): POST http://0.0.0.0:8088/api/ssh

DNS Rebinding protection is disabled throughout for LAN access.
"""

import os
import subprocess
import paramiko
from mcp.server.mcpserver import MCPServer
from mcp.server.transport_security import TransportSecuritySettings
from starlette.responses import JSONResponse
from starlette.requests import Request
import uvicorn

# -------------------------------------------------------------------
# 1. Initialize MCP Server
# -------------------------------------------------------------------
mcp = MCPServer(
    name="ESP-Claw-SSH-Gateway",
    version="1.0.0",
    description="MCP Gateway providing SSH and Sandbox Remote Command Execution capabilities for ESP-Claw"
)

def run_ssh(host: str, command: str, username: str = "root", password: str = "", port: int = 22, key_path: str = "") -> dict:
    try:
        ssh = paramiko.SSHClient()
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

        pkey = None
        if key_path:
            expanded_key = os.path.expanduser(key_path)
            if os.path.exists(expanded_key):
                pkey = paramiko.RSAKey.from_private_key_file(expanded_key)

        ssh.connect(
            hostname=host,
            port=port,
            username=username,
            password=password if password else None,
            pkey=pkey,
            timeout=15
        )

        stdin, stdout, stderr = ssh.exec_command(command)
        out_text = stdout.read().decode("utf-8", errors="replace")
        err_text = stderr.read().decode("utf-8", errors="replace")
        ssh.close()

        result = out_text.strip() if out_text.strip() else err_text.strip()
        if not result:
            result = "[Success: Command executed with no output]"
        return {"status": "success", "output": result}

    except Exception as e:
        return {"status": "error", "message": f"[SSH Error]: {type(e).__name__} - {str(e)}"}

@mcp.tool()
def execute_ssh_command(host: str, command: str, username: str = "root", password: str = "", port: int = 22, key_path: str = "") -> str:
    """Execute a shell command on a remote Linux server or Docker sandbox via SSH.

    Args:
        host: Target server IP address or hostname
        command: The bash command line to execute (e.g., 'uname -a', 'docker ps', 'df -h')
        username: SSH login username (default: 'root')
        password: SSH login password (optional)
        port: SSH port number (default: 22)
        key_path: Absolute path to private key file (optional)
    """
    res = run_ssh(host, command, username, password, port, key_path)
    return res["output"] if res["status"] == "success" else res["message"]

@mcp.tool()
def execute_local_command(command: str) -> str:
    """Execute a local bash command in a local sandbox environment on the Mac host."""
    try:
        proc = subprocess.run(command, shell=True, capture_output=True, text=True, timeout=30)
        output = proc.stdout.strip() if proc.stdout.strip() else proc.stderr.strip()
        return output if output else f"[Success: Code {proc.returncode}]"
    except Exception as e:
        return f"[Error]: {str(e)}"

# -------------------------------------------------------------------
# 2. HTTP REST Endpoint (Bypasses cap_mcp_client C bug using cap_http_request)
# -------------------------------------------------------------------
async def http_ssh_api(request: Request):
    try:
        body = await request.json()
        host = body.get("host")
        command = body.get("command")
        username = body.get("username", "root")
        password = body.get("password", "")
        port = int(body.get("port", 22))
        key_path = body.get("key_path", "")

        if not host or not command:
            return JSONResponse({"status": "error", "message": "Missing 'host' or 'command' in JSON body"}, status_code=400)

        res = run_ssh(host, command, username, password, port, key_path)
        return JSONResponse(res)
    except Exception as e:
        return JSONResponse({"status": "error", "message": str(e)}, status_code=500)

# -------------------------------------------------------------------
# 3. Serve both MCP transports plus the REST fallback from one app
#
# Streamable HTTP is the base app because it owns the session-manager lifespan.
# stateless_http + json_response keep it friendly to the ESP32 client, which opens a
# fresh session (and its own `initialize` handshake) for every single tool call and has
# no reason to parse an SSE frame back.
# Disable DNS Rebinding Protection so LAN IP / hostname Host headers are accepted.
# -------------------------------------------------------------------
sec = TransportSecuritySettings(enable_dns_rebinding_protection=False)
app = mcp.streamable_http_app(
    transport_security=sec,
    stateless_http=True,
    json_response=True,
)
app.routes.extend(mcp.sse_app(transport_security=sec).routes)
app.add_route("/api/ssh", http_ssh_api, methods=["POST"])

if __name__ == "__main__":
    port = int(os.getenv("PORT", "8088"))
    print(f"🚀 ESP-Claw SSH Gateway Tri-Mode Server starting on port {port}")
    print(f"   - MCP Streamable HTTP Endpoint (use from ESP-Claw): http://0.0.0.0:{port}/mcp")
    print(f"   - MCP legacy SSE Endpoint (desktop clients only):   http://0.0.0.0:{port}/sse")
    print(f"   - HTTP REST API Endpoint: POST http://0.0.0.0:{port}/api/ssh")
    uvicorn.run(app, host="0.0.0.0", port=port)
