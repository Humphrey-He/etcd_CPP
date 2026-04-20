#!/usr/bin/env python3
"""
Python client example for etcd MVP with TLS and authentication support.
"""

import grpc
import sys
import etcd_mvp_pb2
import etcd_mvp_pb2_grpc


class EtcdClient:
    def __init__(self, address='localhost:2379', use_tls=False, cert_file=None, key_file=None, ca_file=None):
        if use_tls:
            with open(cert_file, 'rb') as f:
                cert = f.read()
            with open(key_file, 'rb') as f:
                key = f.read()
            with open(ca_file, 'rb') as f:
                ca = f.read()
            credentials = grpc.ssl_channel_credentials(
                root_certificates=ca,
                private_key=key,
                certificate_chain=cert
            )
            self.channel = grpc.secure_channel(address, credentials)
        else:
            self.channel = grpc.insecure_channel(address)

        self.kv_stub = etcd_mvp_pb2_grpc.KVStub(self.channel)
        self.auth_stub = etcd_mvp_pb2_grpc.AuthStub(self.channel)
        self.watch_stub = etcd_mvp_pb2_grpc.WatchStub(self.channel)
        self.lease_stub = etcd_mvp_pb2_grpc.LeaseStub(self.channel)
        self.cluster_stub = etcd_mvp_pb2_grpc.ClusterStub(self.channel)
        self.token = None

    def authenticate(self, username, password):
        """Authenticate and get token."""
        request = etcd_mvp_pb2.AuthenticateRequest(
            username=username,
            password=password
        )
        response = self.auth_stub.Authenticate(request)
        if response.header.code == etcd_mvp_pb2.OK:
            self.token = response.token
            print(f"Authenticated successfully, token: {self.token[:20]}...")
            return True
        else:
            print(f"Authentication failed: {response.header.message}")
            return False

    def _metadata(self):
        """Get metadata with token if available."""
        if self.token:
            return [('token', self.token)]
        return []

    def put(self, key, value, lease=0):
        """Put a key-value pair."""
        request = etcd_mvp_pb2.PutRequest(
            key=key.encode(),
            value=value.encode(),
            lease=lease
        )
        response = self.kv_stub.Put(request, metadata=self._metadata())
        if response.header.code == etcd_mvp_pb2.OK:
            print(f"Put successful, revision: {response.revision}")
            return response.revision
        else:
            print(f"Put failed: {response.header.message}")
            return None

    def get(self, key):
        """Get a key."""
        request = etcd_mvp_pb2.GetRequest(key=key.encode())
        response = self.kv_stub.Get(request, metadata=self._metadata())
        if response.header.code == etcd_mvp_pb2.OK:
            if len(response.kvs) > 0:
                kv = response.kvs[0]
                print(f"Get successful:")
                print(f"  Key: {kv.key.decode()}")
                print(f"  Value: {kv.value.decode()}")
                print(f"  Revision: {response.revision}")
                return kv.value.decode()
            else:
                print("Key not found")
                return None
        else:
            print(f"Get failed: {response.header.message}")
            return None

    def delete(self, key):
        """Delete a key."""
        request = etcd_mvp_pb2.DeleteRequest(key=key.encode())
        response = self.kv_stub.Delete(request, metadata=self._metadata())
        if response.header.code == etcd_mvp_pb2.OK:
            print(f"Delete successful, deleted: {response.deleted}")
            return True
        else:
            print(f"Delete failed: {response.header.message}")
            return False

    def txn(self, compare_list, success_ops, failure_ops):
        """Execute a transaction."""
        request = etcd_mvp_pb2.TxnRequest(
            compare=compare_list,
            success=success_ops,
            failure=failure_ops
        )
        response = self.kv_stub.Txn(request, metadata=self._metadata())
        if response.header.code == etcd_mvp_pb2.OK:
            print(f"Transaction {'succeeded' if response.succeeded else 'failed'}")
            return response
        else:
            print(f"Transaction error: {response.header.message}")
            return None

    def watch(self, key, prefix=False, start_revision=0):
        """Watch a key or prefix."""
        create_req = etcd_mvp_pb2.WatchCreateRequest(
            key=key.encode(),
            prefix=prefix,
            start_revision=start_revision
        )
        request = etcd_mvp_pb2.WatchRequest(create_request=create_req)

        try:
            for response in self.watch_stub.Watch(iter([request]), metadata=self._metadata()):
                if response.header.code == etcd_mvp_pb2.OK:
                    for event in response.events:
                        event_type = "PUT" if event.type == etcd_mvp_pb2.Event.PUT else "DELETE"
                        print(f"Watch event [{event_type}]: {event.kv.key.decode()} = {event.kv.value.decode()}")
                else:
                    print(f"Watch error: {response.header.message}")
                    break
        except KeyboardInterrupt:
            print("\nWatch stopped")

    def lease_grant(self, ttl):
        """Grant a lease."""
        request = etcd_mvp_pb2.LeaseGrantRequest(ttl=ttl)
        response = self.lease_stub.LeaseGrant(request, metadata=self._metadata())
        if response.header.code == etcd_mvp_pb2.OK:
            print(f"Lease granted: ID={response.id}, TTL={response.ttl}")
            return response.id
        else:
            print(f"Lease grant failed: {response.header.message}")
            return None

    def status(self):
        """Get cluster status."""
        request = etcd_mvp_pb2.Empty()
        response = self.cluster_stub.Status(request)
        if response.header.code == etcd_mvp_pb2.OK:
            print(f"Cluster status:")
            print(f"  Leader: {response.leader}")
            print(f"  Revision: {response.revision}")
            return response
        else:
            print(f"Status failed: {response.header.message}")
            return None

    def close(self):
        """Close the channel."""
        self.channel.close()


def main():
    # Example usage
    client = EtcdClient('localhost:2379')

    # Authenticate if auth is enabled
    # client.authenticate('root', 'root')

    # Basic operations
    print("\n=== Put ===")
    client.put('/app/config', 'value1')

    print("\n=== Get ===")
    client.get('/app/config')

    print("\n=== Transaction ===")
    # Compare: check if /app/config value equals 'value1'
    compare = [etcd_mvp_pb2.Compare(
        key=b'/app/config',
        result=etcd_mvp_pb2.EQUAL,
        target=etcd_mvp_pb2.VALUE,
        value=b'value1'
    )]
    # Success: update to 'value2'
    success = [etcd_mvp_pb2.RequestOp(
        put_request=etcd_mvp_pb2.PutRequest(
            key=b'/app/config',
            value=b'value2'
        )
    )]
    # Failure: do nothing
    failure = []
    client.txn(compare, success, failure)

    print("\n=== Get after txn ===")
    client.get('/app/config')

    print("\n=== Lease ===")
    lease_id = client.lease_grant(10)
    if lease_id:
        client.put('/temp/key', 'temp_value', lease=lease_id)

    print("\n=== Status ===")
    client.status()

    print("\n=== Delete ===")
    client.delete('/app/config')

    client.close()


if __name__ == '__main__':
    main()
