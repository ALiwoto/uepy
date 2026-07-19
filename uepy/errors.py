class UepyError(RuntimeError):
    """A user-facing uepy error."""


class DiscoveryError(UepyError):
    """No suitable live Unreal Editor node could be discovered."""


class ProtocolError(UepyError):
    """The Unreal remote execution response was invalid or incomplete."""


class RemoteCommandError(UepyError):
    """Unreal reported that a remote Python command failed."""

