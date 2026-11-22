# Building Releases in GitHub Actions

The **Release** workflow ([`.github/workflows/release-build.yml`](../.github/workflows/release-build.yml))
builds the same playable packages as the local release process, on
GitHub-hosted runners. It needs the supported disc image at CMake configure
time, so the image is kept in a separate **private** repository and downloaded
with a read-only token. The image is never committed here, never uploaded as a
workflow artifact or cache, and never included in a package.

The workflow:

1. Checks that the release tag matches `VERSION`.
2. Downloads the disc image and verifies its size and SHA-256 against
   [`cmake/ImsaGameData.cmake`](../cmake/ImsaGameData.cmake).
3. Configures with `-DIMSA_DISC`, generates the guest code, and verifies
   `build/generated/game_data.c`, `shooter2d_data.c`, and `launchme_data.c`
   exist, so a stub build cannot be published.
4. Builds and runs the game-enabled CTest suite.
5. Packages Linux `.tar.gz` + AppImage, macOS arm64/x86_64 `.dmg`, and the
   Windows portable `.zip`, then checks the package layouts and scans the
   archives for disc data.
6. Writes SHA-256 checksums and creates or updates a **draft** GitHub release.

Keeping the image out of the source repository keeps clones small, avoids Git
LFS bandwidth quotas, and lets the workflow read it without adding anything to
the repository's history.

The four package jobs use the `release` environment, and the publish job gates
the final write. See [Security](#security) for the token and environment setup.

## One-time setup

### 1. Put the image in a private repository

The default location is a private repository named
`<owner>/imsa-wcr-m2-recomp-data` with a release named `game-data` and an
asset named `imsa.iso`. The helper does the whole step and refuses to upload
to a non-private repository:

```sh
tools/seed_game_data_release.sh /path/to/imsa.iso
```

It verifies the image against CMake, creates the private repository and
release when missing, and uploads or replaces the asset. Add `--check` to
verify the image without uploading anything. Equivalent manual commands:

```sh
gh repo create OWNER/imsa-wcr-m2-recomp-data --private --add-readme
gh release create game-data --repo OWNER/imsa-wcr-m2-recomp-data \
    --title "IMSA disc image" --notes "Private copy for CI release builds."
gh release upload game-data /path/to/imsa.iso --repo OWNER/imsa-wcr-m2-recomp-data
```

GitHub allows release assets up to 2 GB; the supported image is about 640 MB.
Keep the repository private. Do not put the image in Git LFS, an Actions
cache, or a workflow artifact: those are visible beyond the release workflow
and can expire.

### 2. Create a read-only token

Create a **fine-grained personal access token** at
<https://github.com/settings/personal-access-tokens/new>:

- **Repository access:** only `OWNER/imsa-wcr-m2-recomp-data`.
- **Repository permissions:** `Contents: Read-only`. No other permissions.
- Give it an expiration and rotate it before it lapses.

Store it as the `release` environment secret in this repository. Create the
environment first if it does not exist:

```sh
gh api repos/OWNER/imsa-wcr-m2-recomp/environments/release >/dev/null 2>&1 || \
    gh api --method PUT repos/OWNER/imsa-wcr-m2-recomp/environments/release
gh secret set GAME_DATA_TOKEN --env release --repo OWNER/imsa-wcr-m2-recomp
```

`gh secret set` prompts for the value without echoing it. You can also use
`--body "$GAME_DATA_TOKEN"` in a script. Do not add it as a repository secret:
the environment gate is what keeps it away from unrelated workflow runs.

### 3. Protect the `release` environment

In **Settings → Environments → release**:

- Optionally add **required reviewers** (yourself or the maintainers who cut
  releases). For a solo maintainer this adds an approval click per release;
  branch and tag restrictions already keep the secret scoped.
- Restrict **deployment branches and tags** to `main` and `v*` tags.

Every package job and the publish job references this environment. If required
reviewers are enabled, approve the pending deployments for the run before the
build starts. The environment secret is not available to jobs that do not
reference the environment, and fork pull requests cannot read it.

### 4. Optional repository variables

The defaults cover the names used above:

| Variable | Default |
| --- | --- |
| `GAME_DATA_REPO` | `<repository owner>/imsa-wcr-m2-recomp-data` |
| `GAME_DATA_RELEASE` | `game-data` |
| `GAME_DATA_ASSET` | `imsa.iso` |

Set one only if the data lives somewhere else:

```sh
gh variable set GAME_DATA_REPO --repo OWNER/imsa-wcr-m2-recomp --body OWNER/other-data-repo
gh variable set GAME_DATA_RELEASE --repo OWNER/imsa-wcr-m2-recomp --body other-release
gh variable set GAME_DATA_ASSET --repo OWNER/imsa-wcr-m2-recomp --body other.iso
```

The asset name is the file name of the uploaded image. The helper prints these
commands when a non-default name is used.

## Running a release

1. Bump [`VERSION`](../VERSION), commit, and merge to the default branch.
2. Push a tag matching the version:

   ```sh
   git tag v1.0.2
   git push origin v1.0.2
   ```

   Or open **Actions → Release → Run workflow**, enter the existing tag
   (`v1.0.2`), and choose whether to create a draft (default) or publish
   immediately.

3. Approve the `release` environment deployments if reviewers are configured.
4. When all four package jobs finish, the publish job creates the draft
   release. Download the assets, run the pre-release checks from the local
   release guide (if you keep one), then publish the draft.

Re-running the workflow for the same tag updates the existing **draft**
release and replaces same-named files. It refuses to touch an already
published release; use a new tag, or delete/unpublish the old release first.

## Storage alternatives

### An HTTPS URL

If the environment secret `GAME_DATA_URL` is set, the workflow prefers it over
the private GitHub release. It downloads with `curl`, using the optional
`GAME_DATA_TOKEN` secret as a bearer token. The URL must return the ISO
itself, not an HTML page.

This works for an authenticated proxy, a Cloudflare Worker/R2 bucket, or an
S3 endpoint that accepts a bearer token. Avoid short-lived presigned URLs:
a URL that expires in a few days will break the next release. For AWS, the
stronger option is GitHub OIDC (`aws-actions/configure-aws-credentials` with
`role-to-assume`) and `aws s3 cp`; that needs a few workflow steps replacing
the URL download, and is not implemented here.

### Self-hosted runners

If the image must never touch GitHub-hosted storage, run the same package
steps on self-hosted runners that already have the image, pass
`-DIMSA_DISC=/path/to/imsa.iso`, and skip the fetch step. The rest of the
workflow (build, test, package checks, release upload) is unchanged. This does
mean maintaining macOS, Windows, and Linux runners.

## Security

- The token is read-only and scoped to one private repository. It cannot push
  code or read this repository's releases.
- It is an **environment** secret, not a repository secret. Combined with
  required reviewers and tag/branch restrictions, only an approved release run
  can use it.
- The workflow triggers only on `v*` tags and manual dispatch. It does not use
  `pull_request_target`, and fork pull requests cannot run it.
- The image is downloaded to `RUNNER_TEMP` on an ephemeral hosted runner. It is
  not uploaded as an artifact or cache. The fetch step refuses a data
  repository that is not private, the packaging step scans ZIP and tar.gz
  archives, and the macOS step mounts the DMG, rejecting any package that
  contains the image or extracted game files.
- Release packages contain the recompiled guest code, as they always have, but
  no disc assets. Players still import their own image on first run.
- Rotate the token before it expires. An expired or revoked token makes the
  download fail with a 404 or an authentication error.

For a longer-lived setup, replace the personal access token with a GitHub App
installed on the data repository only, and use
`actions/create-github-app-token` to mint a short-lived token for the fetch
step. The rest of the workflow is unchanged.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `Could not download ...` | Token exists in the `release` environment, has access to `GAME_DATA_REPO`, and is not expired. The release/tag and asset names match the variables. The release in the private repository is published, not a draft. |
| `Disc image is ... bytes; expected ...` or `SHA-256 ...` | The image is not the supported revision. Compare with `cmake/ImsaGameData.cmake`; the helper checks the same values. |
| `Tag ... does not match VERSION` | Bump `VERSION` before creating the tag. The package file names come from `VERSION`. |
| `Release ... is already published` | The workflow only updates draft releases. Use a new tag, or delete/unpublish the old release first. |
| `Packaging is disabled` | wxWidgets was not found. The Linux job builds a static wxWidgets 3.2 with `-DIMSA_LAUNCHER_FETCH_WXWIDGETS=ON`; keep that argument. |
| `game data found in a package` | A packaging change started including the build tree. The failing paths are printed. Fix `cmake/ImsaPackaging.cmake`; do not remove the check. |
| First run takes a long time | The FFmpeg LGPL build is cached; the first run builds it on each platform. Guest code generation and four full game builds also take time. |

The workflow runs in the public repository, where standard GitHub-hosted
runner usage is free. The private data repository stores one ~640 MB release
asset. Each package job downloads it; the image is not kept in the runner
image or reused across jobs.
