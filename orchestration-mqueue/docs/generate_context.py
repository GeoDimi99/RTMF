import os

def main():
    # The script is in docs/, so project root is one level up
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.abspath(os.path.join(script_dir, ".."))
    output_file = os.path.join(script_dir, "project_context.txt")

    # Configuration for ignored items to keep context clean
    ignore_dirs = {".git", "__pycache__", "build", ".vscode", ".idea", "venv", "env", "cmake-build-debug"}
    ignore_files = {".DS_Store", "project_context.txt"}
    ignore_extensions = {".pyc", ".o", ".obj", ".bin", ".exe", ".so", ".a", ".lib", ".zip", ".tar.gz", ".png", ".jpg"}

    with open(output_file, "w", encoding="utf-8") as out:
        out.write(f"Project Context for: {os.path.basename(project_root)}\n")
        out.write("=" * 50 + "\n\n")

        # 1. Write Directory Structure
        out.write("=== DIRECTORY STRUCTURE ===\n")
        for root, dirs, files in os.walk(project_root):
            # Filter directories in-place
            dirs[:] = [d for d in dirs if d not in ignore_dirs]
            
            level = root.replace(project_root, '').count(os.sep)
            indent = ' ' * 4 * level
            out.write(f"{indent}{os.path.basename(root)}/\n")
            subindent = ' ' * 4 * (level + 1)
            for f in files:
                if f not in ignore_files and not any(f.endswith(ext) for ext in ignore_extensions):
                    out.write(f"{subindent}{f}\n")
        out.write("\n")

        # 2. Write File Contents
        out.write("=== FILE CONTENTS ===\n")
        for root, dirs, files in os.walk(project_root):
            dirs[:] = [d for d in dirs if d not in ignore_dirs]

            for file in files:
                if file in ignore_files or any(file.endswith(ext) for ext in ignore_extensions):
                    continue

                file_path = os.path.join(root, file)
                
                # Skip the output file itself and this script to avoid recursion/duplication
                if os.path.abspath(file_path) == os.path.abspath(output_file):
                    continue
                if os.path.abspath(file_path) == os.path.abspath(__file__):
                    continue

                rel_path = os.path.relpath(file_path, project_root)

                try:
                    with open(file_path, "r", encoding="utf-8") as f:
                        content = f.read()
                    
                    out.write(f"\n--- START FILE: {rel_path} ---\n")
                    out.write(content)
                    out.write(f"\n--- END FILE: {rel_path} ---\n")
                except UnicodeDecodeError:
                    out.write(f"\n--- SKIPPED BINARY FILE: {rel_path} ---\n")
                except Exception as e:
                    out.write(f"\n--- ERROR READING FILE: {rel_path} ({e}) ---\n")

    print(f"Context generated at: {output_file}")

if __name__ == "__main__":
    main()