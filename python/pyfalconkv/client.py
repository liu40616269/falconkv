"""
pyfalconkv.Client - High-level Python wrapper for FalconKV C Extension.

Wraps the _pyfalconkv_internal C extension module's module-level functions
into a clean Client class with an OO interface.
"""

try:
    from pyfalconkv import _pyfalconkv_internal as _internal
except ImportError:
    _internal = None


class Client:
    """FalconKV Python Client.

    Wraps the C Extension module-level functions into an OO interface.
    Internally manages a handle that identifies the FalconKVBridge instance.
    """

    def __init__(self, config_file: str, cache_capacity: int = 100000,
                 worker_id: int = -1):
        if _internal is None:
            raise RuntimeError(
                "_pyfalconkv_internal C extension not found. "
                "Build with: ./build.sh build --with-python"
            )
        self._handle = _internal.Init(config_file, cache_capacity, worker_id)

    def batch_exist_sync(self, keys: list) -> int:
        """Batch check key existence.

        Args:
            keys: List of key strings.

        Returns:
            Hit count.
        """
        return _internal.BatchExistSync(self._handle, keys)

    def batch_put_sync(self, keys: list,
                       data_ptrs: list,
                       sizes: list) -> None:
        """Batch put key-value pairs (synchronous).

        Args:
            keys: List of key strings.
            data_ptrs: List of buffer memory addresses (from tensor.data_ptr()).
            sizes: List of buffer sizes in bytes.
        """
        _internal.BatchPutSync(self._handle, keys, data_ptrs, sizes)

    def batch_get_sync(self, keys: list,
                       data_ptrs: list,
                       sizes: list) -> list:
        """Batch get key-value pairs into pre-allocated buffers.

        Args:
            keys: List of key strings.
            data_ptrs: List of target buffer addresses.
            sizes: List of buffer sizes.

        Returns:
            List of bytes actually read per key. <= 0 means failure.
        """
        return _internal.BatchGetSync(self._handle, keys, data_ptrs, sizes)

    def fire_and_forget_put(self, keys: list,
                            data_ptrs: list,
                            sizes: list,
                            memobjs: list) -> None:
        """Fire-and-forget batch put with automatic Python ref management.

        The C++ backend will call memobj.ref_count_down() on each object
        after the write completes. Callers must call obj.ref_count_up()
        before passing objects here.

        Args:
            keys: List of key strings.
            data_ptrs: List of buffer addresses.
            sizes: List of buffer sizes.
            memobjs: List of Python MemoryObj instances.
        """
        _internal.FireAndForgetPut(
            self._handle, keys, data_ptrs, sizes, memobjs
        )

    def close(self) -> None:
        """Close the client and release all resources."""
        if self._handle is not None:
            _internal.Close(self._handle)
            self._handle = None

    def __del__(self):
        self.close()
