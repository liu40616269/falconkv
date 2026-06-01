"""
Integration tests for LMCache FalconKV adapter.

Tests auto-discovery, URL schema matching, and batched interface support.
"""

import pytest

pytestmark = pytest.mark.integration


def _can_import_connector():
    """Check if FalconKVConnector can be imported (requires LMCache env)."""
    try:
        from pyfalconkv.connector import FalconKVConnector  # noqa: F401
        return True
    except Exception:
        return False


def _can_import_adapter():
    """Check if FalconKVConnectorAdapter can be imported."""
    try:
        from pyfalconkv.adapter import FalconKVConnectorAdapter  # noqa: F401
        return True
    except Exception:
        return False


class TestLMCacheAdapter:
    """IT-LC-001 ~ IT-LC-004: LMCache adapter integration tests."""

    @pytest.mark.skipif(not _can_import_adapter(), reason="pyfalconkv.adapter import failed")
    def test_auto_discovery(self):
        """IT-LC-001: falconkv:// URL schema is recognized by adapter."""
        from pyfalconkv.adapter import FalconKVConnectorAdapter

        adapter = FalconKVConnectorAdapter()
        assert adapter.can_parse("falconkv://localhost:0")
        assert adapter.can_parse("falconkv://10.0.0.1:8900")
        assert not adapter.can_parse("redis://localhost:6379")
        assert not adapter.can_parse("mooncake://localhost")

    @pytest.mark.skipif(not _can_import_adapter(), reason="pyfalconkv.adapter import failed")
    def test_adapter_schema_attribute(self):
        """IT-LC-001: Adapter exposes schema for registration."""
        from pyfalconkv.adapter import FalconKVConnectorAdapter

        adapter = FalconKVConnectorAdapter()
        assert adapter.schema == "falconkv://"

    @pytest.mark.skipif(not _can_import_connector(), reason="pyfalconkv.connector import failed")
    def test_connector_batched_support(self):
        """IT-LC-002 ~ LC-003: Connector reports batched interface support."""
        from pyfalconkv.connector import FalconKVConnector

        # Verify the connector class declares batched support.
        assert hasattr(FalconKVConnector, "support_batched_get")
        assert hasattr(FalconKVConnector, "support_batched_put")
        assert hasattr(FalconKVConnector, "support_batched_contains")
        assert hasattr(FalconKVConnector, "support_batched_get_non_blocking")

    @pytest.mark.skipif(not _can_import_connector(), reason="pyfalconkv.connector import failed")
    def test_connector_has_async_methods(self):
        """IT-LC-004: Connector has async methods for LMCache integration."""
        from pyfalconkv.connector import FalconKVConnector

        assert hasattr(FalconKVConnector, "batched_get")
        assert hasattr(FalconKVConnector, "batched_put")
        assert hasattr(FalconKVConnector, "batched_contains")
        assert hasattr(FalconKVConnector, "batched_get_non_blocking")
        assert hasattr(FalconKVConnector, "close")
