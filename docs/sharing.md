# Sharing your checkout

Run the public-source checks before making a release:

```sh
python3 tools/check_public.py
python3 tools/check.py
```

Publish source, notices, the inert configuration example and original BASIC
source. Keep `build/`, raw backups, credentials, supplied ROMs/disk images and
provisioned firmware out of Git and release archives. A binary built with your
private inputs contains those inputs. Retain the project's license and
[third-party notices](../THIRD_PARTY_NOTICES.md).

Create an empty repository in your own GitHub account, then connect your local
repository. Replace both placeholders below with your chosen account/repository:

```sh
git branch -M main
git remote add origin https://github.com/OWNER/REPOSITORY.git
git push -u origin main
```

GitHub's [existing-source guide](https://docs.github.com/en/migrations/importing-source-code/using-the-command-line-to-import-source-code/adding-locally-hosted-code-to-github)
covers other starting states. Creating or sharing this local project does not
publish it automatically. Include the tested-hardware limitations from the README
when presenting a release.
