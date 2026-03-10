using System;
using System.Runtime.InteropServices;

namespace Mesen.Interop;

// Mirrors GenesisCpuState in Core/Genesis/GenesisTypes.h
public struct GenesisCpuState : BaseState
{
	public UInt64 CycleCount;
	public UInt32 PC;
	public UInt32 SP;   // A7 (active stack pointer)

	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
	public UInt32[] D;

	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
	public UInt32[] A;

	public UInt16 SR;
	[MarshalAs(UnmanagedType.I1)] public bool Stopped;

	public UInt32 USP;  // User Stack Pointer (saved when CPU is in supervisor mode)
}

// Mirrors GenesisVdpState in Core/Genesis/GenesisTypes.h
public struct GenesisVdpState : BaseState
{
	public UInt32 FrameCount;
	public UInt16 HClock;
	public UInt16 VClock;
	public UInt16 Width;
	public UInt16 Height;
	[MarshalAs(UnmanagedType.I1)] public bool PAL;
}

public struct GenesisBackendState
{
	public UInt64 MasterClock;
	public UInt32 FrameWidth;
	public UInt32 FrameHeight;
	public UInt32 ActiveWidth;
	public UInt32 ActiveHeight;
	public UInt32 VdpMclkPos;
	public UInt32 DmaSource;
	public UInt32 DmaLength;
	public UInt16 Scanline;
	public UInt16 HClock;
	public UInt16 VdpStatus;
	public UInt16 DmaFillValue;
	public byte DmaType;
	public byte CpuPendingIrq;
	public byte VintPending;
	public byte HintPending;
	public byte DmaFillPending;
	public byte DisplayEnabled;
	public byte Z80BusRequest;
	public byte Z80Reset;
	public byte Z80BusAck;
	public byte PAL;
}
