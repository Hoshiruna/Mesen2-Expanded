using Avalonia;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Mesen.Utilities;
using Mesen.ViewModels;

namespace Mesen.Windows
{
	public class McpServerWindow : MesenWindow
	{
		private McpServerWindowViewModel Model => (McpServerWindowViewModel)DataContext!;

		public McpServerWindow()
		{
			DataContext = new McpServerWindowViewModel();
			InitializeComponent();
#if DEBUG
			this.AttachDevTools();
#endif
		}

		private void InitializeComponent()
		{
			AvaloniaXamlLoader.Load(this);
		}

		private async void Start_OnClick(object? sender, RoutedEventArgs e)
		{
			if(!Model.TryStart(out string error)) {
				await MessageBox.Show(this, error, "Mesen Expanded - MCP Server", MessageBoxButtons.OK, MessageBoxIcon.Error);
			}
		}

		private void Stop_OnClick(object? sender, RoutedEventArgs e)
		{
			Model.Stop();
		}

		private void CopyUrl_OnClick(object? sender, RoutedEventArgs e)
		{
			ApplicationHelper.GetMainWindow()?.Clipboard?.SetTextAsync(Model.ServerUrl);
		}

		private void Close_OnClick(object? sender, RoutedEventArgs e)
		{
			Close();
		}
	}
}
