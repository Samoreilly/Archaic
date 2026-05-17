with open('src/io/fileloader.h', 'r') as f:
    content = f.read()

content = content.replace('typedef struct {\n    bool scanning;\n    size_t buckets_so_far;\n} scan_status;', 'typedef struct {\n    bool scanning;\n    size_t buckets_so_far;\n    size_t project_count;\n    project_info projects[MAX_PROJECTS];\n} scan_status;')

with open('src/io/fileloader.h', 'w') as f:
    f.write(content)

