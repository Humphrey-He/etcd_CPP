#!/usr/bin/env python3
"""
Generate JSON API documentation from etcd_mvp.proto
"""

import json
import re
from pathlib import Path

def parse_proto_file(proto_path):
    """Parse proto file and extract API documentation"""
    with open(proto_path, 'r', encoding='utf-8') as f:
        content = f.read()

    api_doc = {
        "version": "1.0",
        "package": "etcdmvp",
        "services": []
    }

    # Extract package name
    package_match = re.search(r'package\s+(\w+);', content)
    if package_match:
        api_doc["package"] = package_match.group(1)

    # Extract services and RPCs
    service_pattern = r'service\s+(\w+)\s*\{([^}]+)\}'
    for service_match in re.finditer(service_pattern, content):
        service_name = service_match.group(1)
        service_body = service_match.group(2)

        service_doc = {
            "name": service_name,
            "rpcs": []
        }

        # Extract RPCs
        rpc_pattern = r'rpc\s+(\w+)\s*\(([^)]+)\)\s*returns\s*\((?:stream\s+)?([^)]+)\)'
        for rpc_match in re.finditer(rpc_pattern, service_body):
            rpc_name = rpc_match.group(1)
            request_type = rpc_match.group(2).strip()
            response_type = rpc_match.group(3).strip()
            is_stream = 'stream' in service_body[rpc_match.start():rpc_match.end()]

            rpc_doc = {
                "name": rpc_name,
                "request": request_type,
                "response": response_type,
                "streaming": is_stream,
                "endpoint": f"/{api_doc['package']}.{service_name}/{rpc_name}"
            }
            service_doc["rpcs"].append(rpc_doc)

        api_doc["services"].append(service_doc)

    # Extract message types
    api_doc["messages"] = []
    message_pattern = r'message\s+(\w+)\s*\{([^}]+)\}'
    for msg_match in re.finditer(message_pattern, content):
        msg_name = msg_match.group(1)
        msg_body = msg_match.group(2)

        msg_doc = {
            "name": msg_name,
            "fields": []
        }

        # Extract fields
        field_pattern = r'(?:repeated\s+)?(\w+)\s+(\w+)\s*=\s*(\d+);'
        for field_match in re.finditer(field_pattern, msg_body):
            field_type = field_match.group(1)
            field_name = field_match.group(2)
            field_number = field_match.group(3)
            is_repeated = 'repeated' in msg_body[field_match.start():field_match.end()]

            field_doc = {
                "name": field_name,
                "type": field_type,
                "number": int(field_number),
                "repeated": is_repeated
            }
            msg_doc["fields"].append(field_doc)

        api_doc["messages"].append(msg_doc)

    # Extract enums
    api_doc["enums"] = []
    enum_pattern = r'enum\s+(\w+)\s*\{([^}]+)\}'
    for enum_match in re.finditer(enum_pattern, content):
        enum_name = enum_match.group(1)
        enum_body = enum_match.group(2)

        enum_doc = {
            "name": enum_name,
            "values": []
        }

        # Extract enum values
        value_pattern = r'(\w+)\s*=\s*(\d+);'
        for value_match in re.finditer(value_pattern, enum_body):
            value_name = value_match.group(1)
            value_number = value_match.group(2)

            enum_doc["values"].append({
                "name": value_name,
                "number": int(value_number)
            })

        api_doc["enums"].append(enum_doc)

    return api_doc

def main():
    proto_path = Path(__file__).parent.parent / "proto" / "etcd_mvp.proto"
    output_path = Path(__file__).parent.parent / "docs" / "api_reference.json"

    print(f"Parsing {proto_path}...")
    api_doc = parse_proto_file(proto_path)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(api_doc, f, indent=2, ensure_ascii=False)

    print(f"API documentation generated: {output_path}")
    print(f"Total services: {len(api_doc['services'])}")
    print(f"Total messages: {len(api_doc['messages'])}")
    print(f"Total enums: {len(api_doc['enums'])}")

if __name__ == "__main__":
    main()
