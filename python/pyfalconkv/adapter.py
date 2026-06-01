"""
FalconKVConnectorAdapter - Adapter for creating FalconKVConnector instances.

Auto-discovered by LMCache ConnectorManager via URL schema "falconkv://".
"""

from urllib.parse import parse_qs, urlparse

from pyfalconkv.connector import FalconKVConnector


class FalconKVConnectorAdapter:
    """FalconKV adapter for LMCache auto-discovery.

    URL format: falconkv://localhost:0

    Configuration via extra_config:
    - falconkv_config_file: FalconKV config file path (required)
    - falconkv_cache_capacity: Key descriptor cache capacity (default: 100000)
    - falconkv_async_batch_size: Max concurrent async operations (default: 16)
    - falconkv_fire_and_forget: Enable fire-and-forget put mode (default: True)
    """

    def __init__(self):
        self.schema = "falconkv://"

    def can_parse(self, url: str) -> bool:
        return url.startswith(self.schema)

    def create_connector(self, context) -> object:
        """Create a FalconKVConnector from LMCache context.

        Args:
            context: ConnectorContext with config, loop, local_cpu_backend, etc.

        Returns:
            FalconKVConnector instance
        """
        config = context.config
        extra = config.extra_config if config.extra_config is not None else {}

        config_file = str(extra.get("falconkv_config_file", ""))
        cache_capacity = int(extra.get("falconkv_cache_capacity", 100000))
        async_batch_size = int(extra.get("falconkv_async_batch_size", 16))
        fire_and_forget = bool(extra.get("falconkv_fire_and_forget", True))

        parsed = urlparse(context.url)
        if parsed.query:
            params = parse_qs(parsed.query)
            if "config" in params:
                config_file = params["config"][0]

        return FalconKVConnector(
            config_file=config_file,
            cache_capacity=cache_capacity,
            async_batch_size=async_batch_size,
            fire_and_forget=fire_and_forget,
            loop=context.loop,
            local_cpu_backend=context.local_cpu_backend,
        )
