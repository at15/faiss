#!/usr/bin/env python3
"""
S3 Cache Server TCP Client

This client communicates with the S3 Cache Server using the protocol defined
in s3-cache-server.protocol.claude.md
"""

import socket
import struct
import numpy as np
from typing import Tuple, Dict, Optional


class S3CacheClientError(Exception):
    """Base exception for S3 cache client errors"""
    def __init__(self, code: str, msg: str):
        self.code = code
        self.msg = msg
        super().__init__(f"[{code}] {msg}")


class S3CacheClient:
    """Client for the S3 Cache Server"""

    def __init__(self, host: str = "localhost", port: int = 9001):
        self.host = host
        self.port = port
        self.socket: Optional[socket.socket] = None

    def connect(self):
        """Connect to the server"""
        if self.socket is not None:
            self.close()

        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.connect((self.host, self.port))
        print(f"Connected to {self.host}:{self.port}")

    def close(self):
        """Close the connection"""
        if self.socket:
            self.socket.close()
            self.socket = None
            print("Disconnected")

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _send_command(self, command: str, params: Dict[str, str]):
        """Send a text command with parameters"""
        parts = [command]
        for key, value in params.items():
            parts.append(f"{key}={value}")
        command_line = " ".join(parts) + "\n"
        self.socket.sendall(command_line.encode('utf-8'))

    def _send_binary(self, data: bytes):
        """Send binary data with length prefix"""
        length = len(data)
        self.socket.sendall(struct.pack('<I', length))  # 4-byte little-endian
        self.socket.sendall(data)

    def _recv_exact(self, length: int) -> bytes:
        """Receive exactly length bytes"""
        data = b""
        while len(data) < length:
            chunk = self.socket.recv(length - len(data))
            if not chunk:
                raise ConnectionError("Connection closed while reading data")
            data += chunk
        return data

    def _recv_line(self) -> str:
        """Receive a line of text (until \\n)"""
        line = b""
        while True:
            char = self.socket.recv(1)
            if not char:
                raise ConnectionError("Connection closed while reading line")
            if char == b'\n':
                break
            line += char
            if len(line) > 8192:  # Max line length
                raise ValueError("Line too long")
        return line.decode('utf-8')

    def _recv_binary(self) -> bytes:
        """Receive binary data with length prefix"""
        length_bytes = self._recv_exact(4)
        length = struct.unpack('<I', length_bytes)[0]
        return self._recv_exact(length)

    def _parse_response(self, line: str) -> Dict[str, str]:
        """Parse a response line into key=value pairs"""
        if line.startswith("ERROR "):
            # Parse error response
            parts = line[6:].split(' ', 1)  # Skip "ERROR "
            params = {}
            for part in line[6:].split():
                if '=' in part:
                    key, value = part.split('=', 1)
                    params[key] = value

            code = params.get('code', 'UNKNOWN')
            msg = params.get('msg', 'Unknown error')
            raise S3CacheClientError(code, msg)

        # Parse normal response
        params = {}
        for part in line.split():
            if '=' in part:
                key, value = part.split('=', 1)
                params[key] = value
        return params

    # Command methods

    def echo(self, msg: str) -> str:
        """Test connection with ECHO command"""
        self._send_command("ECHO", {"msg": msg})
        response_line = self._recv_line()
        response = self._parse_response(response_line)
        return response.get("msg", "")

    def load(self, bucket: str, key: str, cluster_data_offset: int) -> int:
        """Load an index from S3"""
        params = {
            "bucket": bucket,
            "key": key,
            "cluster_data_offset": str(cluster_data_offset)
        }
        self._send_command("LOAD", params)
        response_line = self._recv_line()
        response = self._parse_response(response_line)
        return int(response["index"])

    def search(self, index_id: int, query: np.ndarray, k: int) -> Tuple[np.ndarray, np.ndarray]:
        """
        Search an index for k nearest neighbors

        Args:
            index_id: Index ID (from load())
            query: Query vector as float32 numpy array
            k: Number of nearest neighbors to return

        Returns:
            Tuple of (ids, distances) as numpy arrays
        """
        # Validate query vector
        if query.dtype != np.float32:
            query = query.astype(np.float32)

        d = len(query)

        # Send command with binary data
        params = {
            "index": str(index_id),
            "k": str(k),
            "d": str(d)
        }
        self._send_command("SEARCH", params)

        # Send query vector (binary) immediately after command
        query_bytes = query.tobytes()
        self._send_binary(query_bytes)

        # Receive response
        response_line = self._recv_line()
        response = self._parse_response(response_line)
        result_k = int(response["k"])

        # Receive IDs (binary)
        ids_bytes = self._recv_binary()
        ids = np.frombuffer(ids_bytes, dtype=np.int64)

        # Receive distances (binary)
        distances_bytes = self._recv_binary()
        distances = np.frombuffer(distances_bytes, dtype=np.float32)

        # Validate sizes
        if len(ids) != result_k or len(distances) != result_k:
            raise ValueError(f"Expected {result_k} results, got {len(ids)} ids and {len(distances)} distances")

        return ids, distances

    def info_cache(self) -> Dict[str, int]:
        """Get global cache statistics"""
        self._send_command("INFO", {"about": "cache"})
        response_line = self._recv_line()
        response = self._parse_response(response_line)

        return {
            "index_count": int(response.get("index_count", 0)),
            "cache_hits": int(response.get("cache_hits", 0)),
            "cache_misses": int(response.get("cache_misses", 0)),
        }

    def info_index(self, index_id: int) -> Dict[str, int]:
        """Get per-index statistics"""
        params = {
            "about": "index",
            "id": str(index_id)
        }
        self._send_command("INFO", params)
        response_line = self._recv_line()
        response = self._parse_response(response_line)

        return {
            "cluster_count": int(response.get("cluster_count", 0)),
            "cache_hits": int(response.get("cache_hits", 0)),
            "cache_misses": int(response.get("cache_misses", 0)),
            "cached_clusters": int(response.get("cached_clusters", 0)),
        }


def main():
    """Test the client with all commands"""
    print("=" * 60)
    print("S3 Cache Server Client Test")
    print("=" * 60)

    with S3CacheClient() as client:
        # Test 1: ECHO
        print("\n[Test 1] ECHO command")
        try:
            response = client.echo("test_connection")
            print(f"  Response: {response}")
            assert response == "test_connection", "Echo response mismatch"
            print("  ✓ PASS")
        except Exception as e:
            print(f"  ✗ FAIL: {e}")

        # Test 2: LOAD
        print("\n[Test 2] LOAD command")
        try:
            index_id = client.load(
                bucket="my-bucket",
                key="sift1m.ivf",
                cluster_data_offset=3154059
            )
            print(f"  Loaded index ID: {index_id}")
            print("  ✓ PASS")
        except Exception as e:
            print(f"  ✗ FAIL: {e}")
            return

        # Test 3: SEARCH
        print("\n[Test 3] SEARCH command")
        try:
            # Create a dummy query vector (128 dimensions)
            query = np.random.randn(128).astype(np.float32)
            k = 10

            ids, distances = client.search(index_id, query, k)
            print(f"  Returned {len(ids)} results")
            print(f"  IDs: {ids}")
            print(f"  Distances: {distances}")

            assert len(ids) == k, f"Expected {k} results, got {len(ids)}"
            assert len(distances) == k, f"Expected {k} distances, got {len(distances)}"
            print("  ✓ PASS")
        except Exception as e:
            print(f"  ✗ FAIL: {e}")

        # Test 4: INFO (cache)
        print("\n[Test 4] INFO about=cache")
        try:
            stats = client.info_cache()
            print(f"  Index count: {stats['index_count']}")
            print(f"  Cache hits: {stats['cache_hits']}")
            print(f"  Cache misses: {stats['cache_misses']}")
            print("  ✓ PASS")
        except Exception as e:
            print(f"  ✗ FAIL: {e}")

        # Test 5: INFO (index)
        print("\n[Test 5] INFO about=index")
        try:
            stats = client.info_index(index_id)
            print(f"  Cluster count: {stats['cluster_count']}")
            print(f"  Cache hits: {stats['cache_hits']}")
            print(f"  Cache misses: {stats['cache_misses']}")
            print(f"  Cached clusters: {stats['cached_clusters']}")
            print("  ✓ PASS")
        except Exception as e:
            print(f"  ✗ FAIL: {e}")

        # Test 6: Error handling (invalid index)
        print("\n[Test 6] Error handling (INDEX_NOT_FOUND)")
        try:
            query = np.random.randn(128).astype(np.float32)
            ids, distances = client.search(999, query, 10)
            print("  ✗ FAIL: Should have raised an error")
        except S3CacheClientError as e:
            print(f"  Caught error: {e}")
            assert e.code == "INDEX_NOT_FOUND", f"Expected INDEX_NOT_FOUND, got {e.code}"
            print("  ✓ PASS")
        except Exception as e:
            print(f"  ✗ FAIL: Unexpected error: {e}")

        # Test 7: Load another index
        print("\n[Test 7] Load second index")
        try:
            index_id2 = client.load(
                bucket="another-bucket",
                key="another.ivf",
                cluster_data_offset=1000000
            )
            print(f"  Loaded index ID: {index_id2}")
            assert index_id2 == index_id + 1, "Index IDs should be sequential"
            print("  ✓ PASS")
        except Exception as e:
            print(f"  ✗ FAIL: {e}")

        # Test 8: Check global cache stats
        print("\n[Test 8] Global cache stats after loading 2 indexes")
        try:
            stats = client.info_cache()
            print(f"  Index count: {stats['index_count']}")
            assert stats['index_count'] == 2, f"Expected 2 indexes, got {stats['index_count']}"
            print("  ✓ PASS")
        except Exception as e:
            print(f"  ✗ FAIL: {e}")

    print("\n" + "=" * 60)
    print("All tests completed!")
    print("=" * 60)


if __name__ == "__main__":
    main()
