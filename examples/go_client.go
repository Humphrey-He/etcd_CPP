package main

import (
	"context"
	"fmt"
	"log"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"

	pb "path/to/etcd_mvp" // Update this import path
)

type EtcdClient struct {
	conn        *grpc.ClientConn
	kvClient    pb.KVClient
	authClient  pb.AuthClient
	watchClient pb.WatchClient
	leaseClient pb.LeaseClient
	clusterClient pb.ClusterClient
	token       string
}

func NewEtcdClient(address string, useTLS bool, certFile, keyFile, caFile string) (*EtcdClient, error) {
	var opts []grpc.DialOption

	if useTLS {
		creds, err := credentials.NewClientTLSFromFile(caFile, "")
		if err != nil {
			return nil, fmt.Errorf("failed to load TLS credentials: %v", err)
		}
		opts = append(opts, grpc.WithTransportCredentials(creds))
	} else {
		opts = append(opts, grpc.WithTransportCredentials(insecure.NewCredentials()))
	}

	conn, err := grpc.Dial(address, opts...)
	if err != nil {
		return nil, fmt.Errorf("failed to connect: %v", err)
	}

	return &EtcdClient{
		conn:          conn,
		kvClient:      pb.NewKVClient(conn),
		authClient:    pb.NewAuthClient(conn),
		watchClient:   pb.NewWatchClient(conn),
		leaseClient:   pb.NewLeaseClient(conn),
		clusterClient: pb.NewClusterClient(conn),
	}, nil
}

func (c *EtcdClient) Authenticate(username, password string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := c.authClient.Authenticate(ctx, &pb.AuthenticateRequest{
		Username: username,
		Password: password,
	})
	if err != nil {
		return fmt.Errorf("authentication failed: %v", err)
	}

	if resp.Header.Code != pb.ErrorCode_OK {
		return fmt.Errorf("authentication failed: %s", resp.Header.Message)
	}

	c.token = resp.Token
	fmt.Printf("Authenticated successfully, token: %s...\n", c.token[:20])
	return nil
}

func (c *EtcdClient) contextWithToken() context.Context {
	ctx := context.Background()
	if c.token != "" {
		ctx = metadata.AppendToOutgoingContext(ctx, "token", c.token)
	}
	return ctx
}

func (c *EtcdClient) Put(key, value string, lease int64) (int64, error) {
	ctx, cancel := context.WithTimeout(c.contextWithToken(), 5*time.Second)
	defer cancel()

	resp, err := c.kvClient.Put(ctx, &pb.PutRequest{
		Key:   []byte(key),
		Value: []byte(value),
		Lease: lease,
	})
	if err != nil {
		return 0, fmt.Errorf("put failed: %v", err)
	}

	if resp.Header.Code != pb.ErrorCode_OK {
		return 0, fmt.Errorf("put failed: %s", resp.Header.Message)
	}

	fmt.Printf("Put successful, revision: %d\n", resp.Revision)
	return resp.Revision, nil
}

func (c *EtcdClient) Get(key string) (string, error) {
	ctx, cancel := context.WithTimeout(c.contextWithToken(), 5*time.Second)
	defer cancel()

	resp, err := c.kvClient.Get(ctx, &pb.GetRequest{
		Key: []byte(key),
	})
	if err != nil {
		return "", fmt.Errorf("get failed: %v", err)
	}

	if resp.Header.Code != pb.ErrorCode_OK {
		return "", fmt.Errorf("get failed: %s", resp.Header.Message)
	}

	if len(resp.Kvs) == 0 {
		fmt.Println("Key not found")
		return "", nil
	}

	kv := resp.Kvs[0]
	fmt.Printf("Get successful:\n")
	fmt.Printf("  Key: %s\n", string(kv.Key))
	fmt.Printf("  Value: %s\n", string(kv.Value))
	fmt.Printf("  Revision: %d\n", resp.Revision)
	return string(kv.Value), nil
}

func (c *EtcdClient) Delete(key string) error {
	ctx, cancel := context.WithTimeout(c.contextWithToken(), 5*time.Second)
	defer cancel()

	resp, err := c.kvClient.Delete(ctx, &pb.DeleteRequest{
		Key: []byte(key),
	})
	if err != nil {
		return fmt.Errorf("delete failed: %v", err)
	}

	if resp.Header.Code != pb.ErrorCode_OK {
		return fmt.Errorf("delete failed: %s", resp.Header.Message)
	}

	fmt.Printf("Delete successful, deleted: %d\n", resp.Deleted)
	return nil
}

func (c *EtcdClient) Txn(compare []*pb.Compare, success, failure []*pb.RequestOp) (*pb.TxnResponse, error) {
	ctx, cancel := context.WithTimeout(c.contextWithToken(), 5*time.Second)
	defer cancel()

	resp, err := c.kvClient.Txn(ctx, &pb.TxnRequest{
		Compare: compare,
		Success: success,
		Failure: failure,
	})
	if err != nil {
		return nil, fmt.Errorf("transaction failed: %v", err)
	}

	if resp.Header.Code != pb.ErrorCode_OK {
		return nil, fmt.Errorf("transaction error: %s", resp.Header.Message)
	}

	if resp.Succeeded {
		fmt.Println("Transaction succeeded")
	} else {
		fmt.Println("Transaction failed")
	}
	return resp, nil
}

func (c *EtcdClient) Watch(key string, prefix bool, startRevision int64) error {
	ctx := c.contextWithToken()

	stream, err := c.watchClient.Watch(ctx)
	if err != nil {
		return fmt.Errorf("watch failed: %v", err)
	}

	err = stream.Send(&pb.WatchRequest{
		RequestUnion: &pb.WatchRequest_CreateRequest{
			CreateRequest: &pb.WatchCreateRequest{
				Key:           []byte(key),
				Prefix:        prefix,
				StartRevision: startRevision,
			},
		},
	})
	if err != nil {
		return fmt.Errorf("watch send failed: %v", err)
	}

	for {
		resp, err := stream.Recv()
		if err != nil {
			return fmt.Errorf("watch receive failed: %v", err)
		}

		if resp.Header.Code != pb.ErrorCode_OK {
			return fmt.Errorf("watch error: %s", resp.Header.Message)
		}

		for _, event := range resp.Events {
			eventType := "PUT"
			if event.Type == pb.Event_DELETE {
				eventType = "DELETE"
			}
			fmt.Printf("Watch event [%s]: %s = %s\n", eventType, string(event.Kv.Key), string(event.Kv.Value))
		}
	}
}

func (c *EtcdClient) LeaseGrant(ttl int64) (int64, error) {
	ctx, cancel := context.WithTimeout(c.contextWithToken(), 5*time.Second)
	defer cancel()

	resp, err := c.leaseClient.LeaseGrant(ctx, &pb.LeaseGrantRequest{
		Ttl: ttl,
	})
	if err != nil {
		return 0, fmt.Errorf("lease grant failed: %v", err)
	}

	if resp.Header.Code != pb.ErrorCode_OK {
		return 0, fmt.Errorf("lease grant failed: %s", resp.Header.Message)
	}

	fmt.Printf("Lease granted: ID=%d, TTL=%d\n", resp.Id, resp.Ttl)
	return resp.Id, nil
}

func (c *EtcdClient) Status() error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := c.clusterClient.Status(ctx, &pb.Empty{})
	if err != nil {
		return fmt.Errorf("status failed: %v", err)
	}

	if resp.Header.Code != pb.ErrorCode_OK {
		return fmt.Errorf("status failed: %s", resp.Header.Message)
	}

	fmt.Printf("Cluster status:\n")
	fmt.Printf("  Leader: %d\n", resp.Leader)
	fmt.Printf("  Revision: %d\n", resp.Revision)
	return nil
}

func (c *EtcdClient) Close() error {
	return c.conn.Close()
}

func main() {
	client, err := NewEtcdClient("localhost:2379", false, "", "", "")
	if err != nil {
		log.Fatalf("Failed to create client: %v", err)
	}
	defer client.Close()

	// Authenticate if auth is enabled
	// err = client.Authenticate("root", "root")
	// if err != nil {
	// 	log.Fatalf("Authentication failed: %v", err)
	// }

	// Basic operations
	fmt.Println("\n=== Put ===")
	client.Put("/app/config", "value1", 0)

	fmt.Println("\n=== Get ===")
	client.Get("/app/config")

	fmt.Println("\n=== Transaction ===")
	compare := []*pb.Compare{
		{
			Key:    []byte("/app/config"),
			Result: pb.CompareResult_EQUAL,
			Target: pb.CompareTarget_VALUE,
			Value:  []byte("value1"),
		},
	}
	success := []*pb.RequestOp{
		{
			Request: &pb.RequestOp_PutRequest{
				PutRequest: &pb.PutRequest{
					Key:   []byte("/app/config"),
					Value: []byte("value2"),
				},
			},
		},
	}
	client.Txn(compare, success, nil)

	fmt.Println("\n=== Get after txn ===")
	client.Get("/app/config")

	fmt.Println("\n=== Lease ===")
	leaseID, err := client.LeaseGrant(10)
	if err == nil {
		client.Put("/temp/key", "temp_value", leaseID)
	}

	fmt.Println("\n=== Status ===")
	client.Status()

	fmt.Println("\n=== Delete ===")
	client.Delete("/app/config")
}
