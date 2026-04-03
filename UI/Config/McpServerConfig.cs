using ReactiveUI.Fody.Helpers;

namespace Mesen.Config
{
	public class McpServerConfig : BaseConfig<McpServerConfig>
	{
		[Reactive] public ushort Port { get; set; } = 51234;
	}
}
