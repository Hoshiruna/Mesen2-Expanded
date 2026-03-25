using Avalonia.Controls;
using Mesen.Config;
using Mesen.Utilities;
using ReactiveUI.Fody.Helpers;
using System;

namespace Mesen.ViewModels
{
	public class GenesisConfigViewModel : DisposableViewModel
	{
		[Reactive] public GenesisConfig Config { get; set; }
		[Reactive] public GenesisConfig OriginalConfig { get; set; }
		[Reactive] public GenesisConfigTab SelectedTab { get; set; } = 0;

		public Enum[] AvailableRegions => new Enum[] {
			ConsoleRegion.Auto,
			ConsoleRegion.Ntsc,
			ConsoleRegion.NtscJapan,
			ConsoleRegion.Pal
		};

		public Enum[] AvailableControllerTypesP12 => new Enum[] {
			ControllerType.None,
			ControllerType.GenesisController,
			ControllerType.GenesisController3Buttons
		};

		public GenesisConfigViewModel()
		{
			Config = ConfigManager.Config.Genesis;
			OriginalConfig = Config.Clone();

			if(Design.IsDesignMode) {
				return;
			}

			AddDisposable(ReactiveHelper.RegisterRecursiveObserver(Config, (s, e) => { Config.ApplyConfig(); }));
		}
	}

	public enum GenesisConfigTab
	{
		General,
		Input
	}
}
