using System;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;

namespace Mesen.Debugger.Utilities
{
	public static class McpServerManager
	{
		private static readonly object _lock = new();
		private static readonly string _serverExe = Path.Combine(AppContext.BaseDirectory, "MCPServer.exe");
		private static readonly int _parentProcessId = Process.GetCurrentProcess().Id;

		private static DebugPipeServer? _debugPipeServer;
		private static Process? _serverProcess;
		private static ushort _port = 51234;

		public static event EventHandler? StateChanged;

		public static ushort Port {
			get {
				lock(_lock) {
					return _port;
				}
			}
		}

		public static string ServerUrl => $"http://127.0.0.1:{Port}/mcp/";

		public static bool IsRunning {
			get {
				lock(_lock) {
					CleanupExitedProcess_NoLock();
					return IsRunning_NoLock();
				}
			}
		}

		public static bool TryStart(ushort port, out string error)
		{
			error = "";

			lock(_lock) {
				CleanupExitedProcess_NoLock();

				if(IsRunning_NoLock()) {
					if(_port == port) {
						return true;
					}

					error = "The MCP server is already running. Stop it before changing the port.";
					return false;
				}

				if(!File.Exists(_serverExe)) {
					error = $"Could not find MCPServer.exe at:\n{_serverExe}";
					return false;
				}

				if(!IsPortAvailable(port)) {
					error = $"Port {port} is already in use on localhost.";
					return false;
				}

				_debugPipeServer ??= new DebugPipeServer();
				_debugPipeServer.Start();

				var startInfo = new ProcessStartInfo {
					FileName = _serverExe,
					Arguments = $"{port} --parent-pid {_parentProcessId}",
					WorkingDirectory = AppContext.BaseDirectory,
					UseShellExecute = false,
					CreateNoWindow = false,
					WindowStyle = ProcessWindowStyle.Normal
				};

				Process? process = Process.Start(startInfo);
				if(process == null) {
					_debugPipeServer.Stop();
					_debugPipeServer = null;
					error = "Failed to start MCPServer.exe.";
					return false;
				}

				process.EnableRaisingEvents = true;
				process.Exited += ServerProcess_Exited;
				if(process.WaitForExit(250)) {
					process.Exited -= ServerProcess_Exited;
					process.Dispose();
					_debugPipeServer.Stop();
					_debugPipeServer = null;
					error = "MCPServer.exe exited immediately. It may already be running, or the selected port may be unavailable.";
					return false;
				}

				_serverProcess = process;
				_port = port;
			}

			NotifyStateChanged();
			return true;
		}

		public static void Stop()
		{
			Process? process = null;

			lock(_lock) {
				CleanupExitedProcess_NoLock();

				process = _serverProcess;
				_serverProcess = null;
				if(process != null) {
					process.Exited -= ServerProcess_Exited;
				}

				_debugPipeServer?.Stop();
				_debugPipeServer = null;
			}

			if(process != null) {
				try {
					if(!process.HasExited) {
						process.Kill(true);
						process.WaitForExit(1000);
					}
				} catch {
				} finally {
					process.Dispose();
				}
			}

			NotifyStateChanged();
		}

		private static void ServerProcess_Exited(object? sender, EventArgs e)
		{
			lock(_lock) {
				CleanupExitedProcess_NoLock();
				_debugPipeServer?.Stop();
				_debugPipeServer = null;
			}

			NotifyStateChanged();
		}

		private static bool IsPortAvailable(ushort port)
		{
			TcpListener? listener = null;

			try {
				listener = new TcpListener(IPAddress.Loopback, port);
				listener.Start();
				return true;
			} catch {
				return false;
			} finally {
				listener?.Stop();
			}
		}

		private static bool IsRunning_NoLock()
		{
			return _serverProcess != null && !_serverProcess.HasExited;
		}

		private static void CleanupExitedProcess_NoLock()
		{
			if(_serverProcess != null && _serverProcess.HasExited) {
				_serverProcess.Exited -= ServerProcess_Exited;
				_serverProcess.Dispose();
				_serverProcess = null;
			}
		}

		private static void NotifyStateChanged()
		{
			StateChanged?.Invoke(null, EventArgs.Empty);
		}
	}
}
