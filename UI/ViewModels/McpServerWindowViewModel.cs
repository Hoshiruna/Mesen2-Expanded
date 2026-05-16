using Avalonia.Threading;
using Mesen.Config;
using Mesen.Debugger.Utilities;
using ReactiveUI;
using ReactiveUI.Fody.Helpers;
using System;
using System.Reactive.Linq;

namespace Mesen.ViewModels
{
	public class McpServerWindowViewModel : DisposableViewModel
	{
		[Reactive] public int Port { get; set; }
		[Reactive] public bool IsRunning { get; private set; }
		[Reactive] public string ServerUrl { get; private set; } = "";
		[Reactive] public string Status { get; private set; } = "";
		[Reactive] public bool CanEditPort { get; private set; }
		[Reactive] public bool CanStart { get; private set; }
		[Reactive] public bool CanStop { get; private set; }

		public McpServerWindowViewModel()
		{
			Port = ConfigManager.Config.McpServer.Port;
			RefreshDerivedState();

			AddDisposable(this.WhenAnyValue(x => x.Port).Subscribe(_ => RefreshDerivedState()));
			McpServerManager.StateChanged += McpServerManager_StateChanged;
		}

		public bool TryStart(out string error)
		{
			error = "";

			if(Port < 1 || Port > 65535) {
				error = "Port must be between 1 and 65535.";
				return false;
			}

			if(!McpServerManager.TryStart((ushort)Port, out error)) {
				RefreshDerivedState();
				return false;
			}

			ConfigManager.Config.McpServer.Port = (ushort)Port;
			ConfigManager.Config.Save();
			RefreshDerivedState();
			return true;
		}

		public void Stop()
		{
			McpServerManager.Stop();
			RefreshDerivedState();
		}

		protected override void DisposeView()
		{
			McpServerManager.StateChanged -= McpServerManager_StateChanged;
		}

		private void McpServerManager_StateChanged(object? sender, EventArgs e)
		{
			Dispatcher.UIThread.Post(RefreshDerivedState);
		}

		private void RefreshDerivedState()
		{
			IsRunning = McpServerManager.IsRunning;
			ServerUrl = $"http://127.0.0.1:{Port}/mcp/";
			Status = IsRunning ? $"Running on {McpServerManager.ServerUrl}" : "Stopped";
			CanEditPort = !IsRunning;
			CanStart = !IsRunning;
			CanStop = IsRunning;
		}
	}
}
