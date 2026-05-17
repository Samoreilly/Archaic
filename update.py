import re

with open('ipc/protocol.h', 'r') as f:
    content = f.read()

replacement = """#define MAX_PROJECTS 100

typedef struct {
    char path[512];
    char language[32];
} ipc_project_info;

typedef struct {
    int32_t scanning;
    uint64_t buckets_so_far;
    uint32_t project_count;
    ipc_project_info projects[MAX_PROJECTS];
} __attribute__((packed)) ipc_scan_status_resp;"""

content = re.sub(r'typedef struct \{\n    int32_t scanning;\n    uint64_t buckets_so_far;\n\} __attribute__\(\(packed\)\) ipc_scan_status_resp;', replacement, content)

with open('ipc/protocol.h', 'w') as f:
    f.write(content)

with open('src/threadmanager.h', 'r') as f:
    content = f.read()

content = content.replace('#define SCANNER_MAX_IGNORE_LEN 128', '#define SCANNER_MAX_IGNORE_LEN 128\n#define MAX_PROJECTS 100\n\ntypedef struct {\n    char path[512];\n    char language[32];\n} project_info;\n')

with open('src/threadmanager.h', 'w') as f:
    f.write(content)

