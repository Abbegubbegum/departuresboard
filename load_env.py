Import("env")
import os


def load_env_file(path):
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip().strip('"').strip("'")
    return values


env_path = os.path.join(env["PROJECT_DIR"], ".env")
env_vars = load_env_file(env_path)

if not env_vars:
    print("load_env.py: no .env file found (or it's empty) - copy .env.example to .env and fill in secrets")

for key, value in env_vars.items():
    env.Append(BUILD_FLAGS=[f'-D{key}=\\"{value}\\"'])
