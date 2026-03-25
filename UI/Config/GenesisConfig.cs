using Mesen.Interop;
using ReactiveUI.Fody.Helpers;
using System;
using System.Runtime.InteropServices;

namespace Mesen.Config;

public class GenesisConfig : BaseConfig<GenesisConfig>
{
	[Reactive] public ControllerConfig Port1 { get; set; } = new();
	[Reactive] public ControllerConfig Port2 { get; set; } = new();

	[ValidValues(ConsoleRegion.Auto, ConsoleRegion.Ntsc, ConsoleRegion.NtscJapan, ConsoleRegion.Pal)]
	[Reactive] public ConsoleRegion Region { get; set; } = ConsoleRegion.Auto;

	[Reactive] public GenesisCoreType CoreType { get; set; } = GenesisCoreType.Native;

	public void ApplyConfig()
	{
		ConfigApi.SetGenesisConfig(new InteropGenesisConfig() {
			Port1 = Port1.ToInterop(),
			Port2 = Port2.ToInterop(),
			Region = Region,
			CoreType = CoreType
		});
	}

	internal void InitializeDefaults(DefaultKeyMappingType defaultMappings)
	{
		Port1.InitDefaults(defaultMappings, ControllerType.GenesisController);
		Port2.Type = ControllerType.None;
	}
}

[StructLayout(LayoutKind.Sequential)]
public struct InteropGenesisConfig
{
	public InteropControllerConfig Port1;
	public InteropControllerConfig Port2;

	public ConsoleRegion Region;
	public GenesisCoreType CoreType;
}

public enum GenesisCoreType
{
	Native = 0
}
