using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Mesen.Interop;
using Mesen.Utilities;
using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;

namespace Mesen.Windows
{
	public partial class SelectBinFileConsoleTypeWindow : MesenWindow
	{
		public SelectBinFileConsoleTypeWindow()
		{
			InitializeComponent();
#if DEBUG
			this.AttachDevTools();
#endif
		}

		private void InitializeComponent()
		{
			AvaloniaXamlLoader.Load(this);
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			if(e.Key == Key.Enter) {
				AcceptSelection();
			} else if(e.Key == Key.Escape) {
				Close();
			}
			base.OnKeyDown(e);
		}

		public static async Task<BinFileConsoleTypeSelection?> Show(ResourcePath romPath)
		{
			SelectBinFileConsoleTypeViewModel model = new(romPath);
			SelectBinFileConsoleTypeWindow wnd = new() { DataContext = model };

			Window? parent = ApplicationHelper.GetMainWindow();
			if(parent == null) {
				return null;
			}

			wnd.WindowStartupLocation = WindowStartupLocation.CenterOwner;
			await wnd.ShowDialog(parent);

			if(model.Cancelled || model.SelectedConsoleType == null) {
				return null;
			}

			return new BinFileConsoleTypeSelection(model.SelectedConsoleType.ConsoleType, model.RememberSelection);
		}

		private void AcceptSelection()
		{
			if(DataContext is SelectBinFileConsoleTypeViewModel model && model.SelectedConsoleType != null) {
				model.Cancelled = false;
				Close();
			}
		}

		private void OnOkClick(object sender, RoutedEventArgs e)
		{
			AcceptSelection();
		}

		private void OnCancelClick(object sender, RoutedEventArgs e)
		{
			Close();
		}
	}

	public class SelectBinFileConsoleTypeViewModel
	{
		public List<BinFileConsoleTypeOption> ConsoleTypes { get; } = new()
		{
			new(ConsoleType.Genesis, "Genesis / Mega Drive"),
			new(ConsoleType.Sms, "Master System"),
			new(ConsoleType.PcEngine, "PC Engine / TurboGrafx-16"),
			new(ConsoleType.Snes, "Super Nintendo"),
			new(ConsoleType.Nes, "NES / Famicom"),
			new(ConsoleType.Gameboy, "Game Boy / Game Boy Color"),
			new(ConsoleType.Gba, "Game Boy Advance"),
			new(ConsoleType.Ws, "WonderSwan")
		};

		public BinFileConsoleTypeOption? SelectedConsoleType { get; set; }
		public bool RememberSelection { get; set; }
		public bool Cancelled { get; set; } = true;
		public string FileName { get; }

		public SelectBinFileConsoleTypeViewModel(ResourcePath romPath)
		{
			FileName = Path.GetFileName(romPath.FileName);
			SelectedConsoleType = ConsoleTypes[0];
		}
	}

	public class BinFileConsoleTypeOption
	{
		public ConsoleType ConsoleType { get; }
		public string DisplayName { get; }

		public BinFileConsoleTypeOption(ConsoleType consoleType, string displayName)
		{
			ConsoleType = consoleType;
			DisplayName = displayName;
		}
	}

	public record BinFileConsoleTypeSelection(ConsoleType ConsoleType, bool RememberSelection);
}
