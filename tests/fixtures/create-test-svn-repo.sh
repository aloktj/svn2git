#!/bin/sh
# Create a minimal SVN repository with a standard layout for testing.
#
# usage: create-test-svn-repo.sh <target-directory>
#
# Produces:
#   <target-directory>/repo          the SVN repository (svnadmin create)
#   file://<target-directory>/repo   URL to use with svn2git tooling
#
# History created (6 revisions, 2 authors):
#   r1  jsmith        create trunk/branches/tags layout
#   r2  jsmith        add trunk/README.md and trunk/src/main.c
#   r3  mmustermann   modify trunk/src/main.c
#   r4  jsmith        copy trunk -> branches/release-1.0
#   r5  mmustermann   fix on branches/release-1.0
#   r6  jsmith        copy branches/release-1.0 -> tags/v1.0.0

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <target-directory>" >&2
    exit 2
fi

TARGET="$1"
REPO="$TARGET/repo"
WC="$TARGET/wc"

mkdir -p "$TARGET"
svnadmin create "$REPO"
URL="file://$(cd "$TARGET" && pwd)/repo"

# Commit as different authors via --username (file:// allows any name).
svn checkout -q --non-interactive "$URL" "$WC"

cd "$WC"
mkdir -p trunk branches tags
svn add -q trunk branches tags
svn commit -q --username jsmith -m "r1: create standard layout"

mkdir -p trunk/src
printf '# Test Project\n' > trunk/README.md
printf 'int main(void) { return 0; }\n' > trunk/src/main.c
svn add -q trunk/README.md trunk/src
svn commit -q --username jsmith -m "r2: add initial sources"

printf 'int main(void) { return 42; }\n' > trunk/src/main.c
svn commit -q --username mmustermann -m "r3: change exit code"

svn update -q
svn copy -q trunk branches/release-1.0
svn commit -q --username jsmith -m "r4: branch release-1.0"

printf 'int main(void) { return 1; }\n' > branches/release-1.0/src/main.c
svn commit -q --username mmustermann -m "r5: hotfix on release branch"

svn update -q
svn copy -q branches/release-1.0 tags/v1.0.0
svn commit -q --username jsmith -m "r6: tag v1.0.0"

echo "$URL"
