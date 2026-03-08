// MCPServer.exe — Standalone MCP HTTP server for Mesen2 Expanded.
//
// Architecture:
//   Mesen.exe  <──named pipe "MesenDebug"──>  MCPServer.exe  <──HTTP──>  AI client
//
// The emulator exposes a named-pipe JSON-RPC endpoint (DebugPipeServer.cs).
// This process connects to that pipe, listens on HTTP 127.0.0.1:51234, and
// forwards every POST body to the pipe, returning the pipe response as the
// HTTP response.  All MCP protocol logic lives on the C# side.
//
// Usage: MCPServer.exe [port]   (default port 51234)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2def.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────
static const char* PIPE_NAME = "\\\\.\\pipe\\MesenDebug";
static int g_port = 51234;

// ─────────────────────────────────────────────────────────────────────────────
// Named-pipe client
// ─────────────────────────────────────────────────────────────────────────────
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_pipeLock;

static bool PipeIsOpen()
{
	return g_pipe != nullptr && g_pipe != INVALID_HANDLE_VALUE;
}

static void PipeClose()
{
	if(PipeIsOpen()) {
		CloseHandle(g_pipe);
	}
	if(g_pipe != INVALID_HANDLE_VALUE) {
		g_pipe = INVALID_HANDLE_VALUE;
	}
}

// Blocking connect; retries indefinitely until the pipe server appears.
static void PipeConnect()
{
	PipeClose();
	while(true) {
		g_pipe = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
			0, nullptr, OPEN_EXISTING, 0, nullptr);
		if(g_pipe != INVALID_HANDLE_VALUE)
			return;

		DWORD err = GetLastError();
		if(err == ERROR_PIPE_BUSY)
			WaitNamedPipeA(PIPE_NAME, 2000);
		else {
			fprintf(stderr, "[MCPServer] Waiting for Mesen (pipe not found)...\n");
			Sleep(1000);
		}
	}
}

// Read one newline-terminated line from the pipe into `out`.
static bool PipeReadLine(std::string& out)
{
	out.clear();
	char c = 0;
	DWORD read = 0;
	while(PipeIsOpen() && ReadFile(g_pipe, &c, 1, &read, nullptr) && read == 1) {
		if(c == '\n') return true;
		if(c != '\r') out += c;
	}
	return false; // EOF / error
}

// Send request JSON to pipe, receive response JSON (line-delimited).
// Returns compact JSON string.  On pipe error, reconnects once and retries.
static std::string PipeRequest(const std::string& json)
{
	EnterCriticalSection(&g_pipeLock);

	for(int attempt = 0; attempt < 2; ++attempt) {
		if(!PipeIsOpen())
			PipeConnect();
		if(!PipeIsOpen()) {
			continue;
		}

		// Strip any embedded newlines so the line protocol stays intact.
		std::string msg;
		msg.reserve(json.size() + 1);
		for(char c : json)
			if(c != '\n' && c != '\r') msg += c;
		msg += '\n';

		DWORD written = 0;
		if(!WriteFile(g_pipe, msg.data(), (DWORD)msg.size(), &written, nullptr)) {
			PipeClose();
			continue;
		}

		std::string response;
		if(PipeReadLine(response)) {
			LeaveCriticalSection(&g_pipeLock);
			return response;
		}

		PipeClose();
	}

	LeaveCriticalSection(&g_pipeLock);
	return R"({"jsonrpc":"2.0","id":0,"error":{"code":-32603,"message":"Emulator not running"}})";
}

// ─────────────────────────────────────────────────────────────────────────────
// Minimal HTTP/1.1 server (single-file, no dependencies)
// ─────────────────────────────────────────────────────────────────────────────

struct HttpRequest {
	std::string method;
	std::string path;
	std::string body;
	bool valid = false;
};

// Receive until we have the complete HTTP request (headers + full body).
static std::string RecvAll(SOCKET s)
{
	std::string data;
	char buf[8192];
	while(true) {
		int r = recv(s, buf, sizeof(buf), 0);
		if(r <= 0) break;
		data.append(buf, r);

		size_t headerEnd = data.find("\r\n\r\n");
		if(headerEnd == std::string::npos) continue;

		// Find Content-Length
		std::string hdr = data.substr(0, headerEnd);
		std::string hdrLow = hdr;
		std::transform(hdrLow.begin(), hdrLow.end(), hdrLow.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});

		size_t clPos = hdrLow.find("content-length:");
		if(clPos == std::string::npos) break; // no body

		size_t valueStart = clPos + 15;
		while(valueStart < hdrLow.size() && hdrLow[valueStart] == ' ') ++valueStart;
		int contentLen = std::stoi(hdrLow.substr(valueStart));

		size_t bodyStart = headerEnd + 4;
		while((int)(data.size() - bodyStart) < contentLen) {
			r = recv(s, buf, sizeof(buf), 0);
			if(r <= 0) break;
			data.append(buf, r);
		}
		break;
	}
	return data;
}

static HttpRequest ParseRequest(const std::string& raw)
{
	HttpRequest req;
	size_t lineEnd = raw.find("\r\n");
	if(lineEnd == std::string::npos) return req;

	std::string reqLine = raw.substr(0, lineEnd);
	size_t sp1 = reqLine.find(' ');
	size_t sp2 = reqLine.rfind(' ');
	if(sp1 == std::string::npos || sp1 == sp2) return req;

	req.method = reqLine.substr(0, sp1);
	req.path   = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);

	size_t headerEnd = raw.find("\r\n\r\n");
	if(headerEnd != std::string::npos)
		req.body = raw.substr(headerEnd + 4);

	req.valid = true;
	return req;
}

static void SendHttp(SOCKET s, int status, const std::string& body)
{
	const char* statusText =
		status == 200 ? "200 OK" :
		status == 204 ? "204 No Content" :
		status == 405 ? "405 Method Not Allowed" : "400 Bad Request";

	char hdr[512];
	int hdrLen = snprintf(hdr, sizeof(hdr),
		"HTTP/1.1 %s\r\n"
		"Content-Type: application/json\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Access-Control-Allow-Methods: POST, OPTIONS\r\n"
		"Access-Control-Allow-Headers: Content-Type\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"\r\n",
		statusText, body.size());

	send(s, hdr, hdrLen, 0);
	if(!body.empty())
		send(s, body.data(), (int)body.size(), 0);
}

static void HandleClient(SOCKET client)
{
	std::string raw = RecvAll(client);
	if(raw.empty()) { closesocket(client); return; }

	HttpRequest req = ParseRequest(raw);
	if(!req.valid) { closesocket(client); return; }

	if(req.method == "OPTIONS") {
		SendHttp(client, 204, "");
	} else if(req.method != "POST") {
		SendHttp(client, 405, R"({"error":"Only POST is supported"})");
	} else if(req.body.empty()) {
		SendHttp(client, 400,
			R"({"jsonrpc":"2.0","id":0,"error":{"code":-32700,"message":"Empty body"}})");
	} else {
		std::string result = PipeRequest(req.body);
		SendHttp(client, 200, result);
	}

	closesocket(client);
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
	if(argc >= 2)
		g_port = std::atoi(argv[1]);

	printf("[MCPServer] Mesen2 MCP Server\n");
	printf("[MCPServer] Connecting to Mesen on pipe %s ...\n", PIPE_NAME);

	WSADATA wsa{};
	if(const int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsa); wsaResult != 0) {
		fprintf(stderr, "[MCPServer] WSAStartup() failed: %d\n", wsaResult);
		return 1;
	}
	InitializeCriticalSection(&g_pipeLock);

	// Block here until Mesen is running.
	PipeConnect();
	printf("[MCPServer] Connected to Mesen.\n");

	// Start HTTP server.
	SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(srv == INVALID_SOCKET) {
		fprintf(stderr, "[MCPServer] socket() failed: %d\n", WSAGetLastError());
		return 1;
	}

	int opt = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

	sockaddr_in addr{};
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons((u_short)g_port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if(bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
		fprintf(stderr, "[MCPServer] bind() failed: %d\n", WSAGetLastError());
		return 1;
	}
	listen(srv, SOMAXCONN);

	printf("[MCPServer] Listening on http://127.0.0.1:%d/mcp/\n", g_port);
	printf("[MCPServer] To connect from Claude Code:\n");
	printf("  claude mcp add --transport http mesen-debugger http://127.0.0.1:%d/mcp/\n", g_port);

	while(true) {
		SOCKET client = accept(srv, nullptr, nullptr);
		if(client == INVALID_SOCKET) break;
		std::thread([client]() { HandleClient(client); }).detach();
	}

	closesocket(srv);
	WSACleanup();
	DeleteCriticalSection(&g_pipeLock);
	return 0;
}
