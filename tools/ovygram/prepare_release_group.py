"""Prepare pinned Windows dependencies for the optimized client."""
import os
from pathlib import Path
import sys

from dependency_cache import group_for
from prepare_group import grouped_recipe


def release_recipe(text, filename, group):
    if group == 'qt':
        # Preserve upstream's patching and flags. Build only the variant we link
        # and omit the extra Qt debug symbols to stay within runner disk limits.
        changes = {
            'SET CONFIGURATIONS=-debug-and-release': 'SET CONFIGURATIONS=-release',
            '        -force-debug-info ^\n': '',
            ('    cmake --build . --config Debug\n'
             '    cmake --install . --config Debug\n'
             '    cmake --build .\n'
             '    cmake --install .\n'): ('    cmake --build . --config Release\n'
                                          '    cmake --install . --config Release\n'),
        }
        if any(text.count(original) != 1 for original in changes):
            raise RuntimeError('Pinned Qt recipe changed. Review the Release-only configuration.')
        for original, updated in changes.items():
            text = text.replace(original, updated)
    return grouped_recipe(text, filename, group)


def main():
    group, filename = sys.argv[1:3]
    if group not in ('foundation', 'qt', 'extras'):
        raise ValueError('Unknown dependency group.')
    if group == 'foundation':
        # The portable bundle does not need Breakpad's ATL-based symbol tool.
        os.environ['SKIP_DUMPSYMS'] = '1'
    recipe = Path(filename).resolve()
    sys.path.insert(0, str(recipe.parent))
    sys.argv = [str(recipe), 'qt6', 'silent']
    namespace = {'__file__': str(recipe), '__name__': '__main__',
                 '_desktop_group_for': group_for}
    exec(release_recipe(recipe.read_text(encoding='utf-8'), str(recipe), group), namespace)


if __name__ == '__main__':
    main()
