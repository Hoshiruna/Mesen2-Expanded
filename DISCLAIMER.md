# MCP and AI Assistance Disclaimer

This disclaimer applies to the MCP server, bridge tooling, and AI-assisted debugging workflows used with Mesen Expanded.

## Scope

Mesen Expanded may be used together with local or third-party large language model (LLM) tools through the included MCP/debugger integration. This disclaimer covers:

- the built-in MCP/debugger interfaces
- bridge tools used to connect external clients
- actions proposed or performed by connected AI/LLM clients

By enabling or connecting to this functionality, you acknowledge and accept the following.

## No Warranty

The MCP server is provided "as is" without warranty of any kind, express or implied. The project maintainers make no guarantees regarding the reliability, accuracy, or availability of the MCP server or any responses produced by connected LLM clients.

## User Responsibility

You are solely responsible for:

- Deciding which LLM clients or services to connect to the MCP server
- Reviewing and validating any actions, code, or modifications that an LLM
  client performs through the debugger interface
- Ensuring that your use of third-party LLM services complies with their
  respective terms of service and applicable laws

## Limitation of Liability

To the maximum extent permitted by applicable law, the project maintainers shall not be liable for any direct, indirect, incidental, special, or consequential damages arising from:

- Use of or inability to use the MCP server
- Actions taken by a connected LLM client, including but not limited to unintended memory writes, breakpoint changes, or execution control
- Data transmitted to or processed by third-party LLM services
- Any reliance on information or suggestions provided by a connected LLM

When you use the built-in MCP server in Mesen Expanded to connect the debugger to an LLM, you are deemed to assume all liability arising from the use of AI in that workflow.

## Third-Party Services

The MCP server facilitates communication with external LLM services that are not developed, operated, or endorsed by the Mesen Expanded project. The availability, behavior, and policies of these services are governed entirely by their respective providers.

## No Endorsement

References to AI tools, MCP clients, or third-party model providers are provided for interoperability only. They do not imply endorsement, certification, or responsibility for those tools or services.

## Security

The MCP server binds to `127.0.0.1` (localhost) by default and is not intended to be exposed to untrusted networks. If you modify the bind address or forward the port, you do so at your own risk.
