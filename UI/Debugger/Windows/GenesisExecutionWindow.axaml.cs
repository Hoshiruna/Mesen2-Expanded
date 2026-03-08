using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using Mesen.Interop;
using Mesen.Utilities;
using System;

namespace Mesen.Debugger.Windows;

public class GenesisExecutionWindow : MesenWindow
{
	private static GenesisExecutionWindow? _instance;

	private readonly DispatcherTimer _timer;

	// Cached control references — populated once after InitializeComponent()
	private readonly TextBlock _statusText;
	private readonly TextBlock _cyclesText;
	private readonly TextBlock _instructionText;
	private readonly TextBlock[] _d = new TextBlock[8];
	private readonly TextBlock[] _a = new TextBlock[7];
	private readonly TextBlock _uspText;
	private readonly TextBlock _sspText;
	private readonly TextBlock _pcText;
	private readonly TextBlock _srText;
	private readonly TextBlock _srFlagsText;
	private readonly Button _pauseBtn;
	private readonly Button _resumeBtn;

	public GenesisExecutionWindow()
	{
		InitializeComponent();

		_statusText      = this.GetControl<TextBlock>("StatusText");
		_cyclesText      = this.GetControl<TextBlock>("CyclesText");
		_instructionText = this.GetControl<TextBlock>("InstructionText");
		for(int i = 0; i < 8; i++) _d[i] = this.GetControl<TextBlock>($"D{i}");
		for(int i = 0; i < 7; i++) _a[i] = this.GetControl<TextBlock>($"A{i}");
		_uspText    = this.GetControl<TextBlock>("UspText");
		_sspText    = this.GetControl<TextBlock>("SspText");
		_pcText     = this.GetControl<TextBlock>("PcText");
		_srText     = this.GetControl<TextBlock>("SrText");
		_srFlagsText = this.GetControl<TextBlock>("SrFlagsText");
		_pauseBtn   = this.GetControl<Button>("PauseBtn");
		_resumeBtn  = this.GetControl<Button>("ResumeBtn");

		_timer = new DispatcherTimer(
			TimeSpan.FromMilliseconds(100),
			DispatcherPriority.Normal,
			OnTimer
		);
		_timer.Start();
	}

	private void InitializeComponent()
	{
		AvaloniaXamlLoader.Load(this);
	}

	public static GenesisExecutionWindow GetOrOpenWindow()
	{
		if(_instance == null || !_instance.IsVisible) {
			_instance = new GenesisExecutionWindow();
			_instance.Show();
		} else {
			_instance.BringToFront();
		}
		return _instance;
	}

	protected override void OnClosed(EventArgs e)
	{
		_timer.Stop();
		_instance = null;
		base.OnClosed(e);
	}

	private void OnTimer(object? sender, EventArgs e)
	{
		try {
			bool paused = EmuApi.IsPaused();
			_statusText.Text  = paused ? "Paused" : "Running";
			_pauseBtn.IsEnabled  = !paused;
			_resumeBtn.IsEnabled = paused;

			GenesisCpuState cpu = DebugApi.GetCpuState<GenesisCpuState>(CpuType.GenesisMain);

			_cyclesText.Text = cpu.CycleCount.ToString("N0");

			// Current instruction — fetch one disassembly line at current PC
			CodeLineData[] lines = DebugApi.GetDisassemblyOutput(CpuType.GenesisMain, cpu.PC, 1);
			_instructionText.Text = lines.Length > 0
				? $"${cpu.PC:X6}  {lines[0].Text}"
				: $"${cpu.PC:X6}";

			// Data registers
			for(int i = 0; i < 8; i++) {
				_d[i].Text = $"D{i}: ${cpu.D[i]:X8}";
			}

			// Address registers A0-A6
			for(int i = 0; i < 7; i++) {
				_a[i].Text = $"A{i}: ${cpu.A[i]:X8}";
			}

			// Supervisor mode: A[7] = SSP, USP field holds saved USP
			// User mode: A[7] = USP, USP field also = A[7]
			bool supervisor = (cpu.SR & 0x2000) != 0;
			uint ssp = supervisor ? cpu.A[7] : 0;
			uint usp = cpu.USP;

			_uspText.Text = $"USP: ${usp:X8}";
			_sspText.Text = $"SSP: ${ssp:X8}";
			_pcText.Text  = $"PC:  ${cpu.PC:X6}";
			_srText.Text  = $"SR:  ${cpu.SR:X4}";

			// SR flag decode:
			//   bit 15: T (trace)  bit 13: S (supervisor)  bits 10-8: IPL
			//   bit 4: X  bit 3: N  bit 2: Z  bit 1: V  bit 0: C
			int  ipl = (cpu.SR >> 8) & 7;
			bool t   = (cpu.SR & 0x8000) != 0;
			bool s   = (cpu.SR & 0x2000) != 0;
			bool x   = (cpu.SR & 0x0010) != 0;
			bool n   = (cpu.SR & 0x0008) != 0;
			bool z   = (cpu.SR & 0x0004) != 0;
			bool v   = (cpu.SR & 0x0002) != 0;
			bool c   = (cpu.SR & 0x0001) != 0;

			_srFlagsText.Text =
				$"T={B(t)}  S={B(s)}  IPL={ipl}  X={B(x)}  N={B(n)}  Z={B(z)}  V={B(v)}  C={B(c)}";
		} catch {
			// Emulator not running or Genesis not active — leave display as-is
		}
	}

	private static char B(bool v) => v ? '1' : '0';

	private void OnPauseClick(object? sender, RoutedEventArgs e)
	{
		EmuApi.Pause();
	}

	private void OnStepClick(object? sender, RoutedEventArgs e)
	{
		DebugApi.Step(CpuType.GenesisMain, 1, StepType.Step);
	}

	private void OnResumeClick(object? sender, RoutedEventArgs e)
	{
		EmuApi.Resume();
	}
}
