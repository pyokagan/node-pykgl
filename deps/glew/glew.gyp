{
  'variables': {
    'use_system_glew%': 0,
  },
  'conditions': [
    ['use_system_glew == 0', {
      'targets': [
        {
          'target_name': 'glew',
          'type': 'static_library',
          'defines': ['GLEW_NO_GLU', 'GLEW_STATIC'],
          'include_dirs': ['glew/include'],
          'sources': ['glew/src/glew.c'],
          'direct_dependent_settings': {
            'defines': ['GLEW_STATIC'],
            'include_dirs': ['glew/include'],
          },
          'conditions': [
            ['OS == "linux"', {
              'direct_dependent_settings': {
                'link_settings': {
                  # y u no static link all of these dependencies ;)
                  'ldflags': ['<!@(pkg-config --libs-only-L --libs-only-other xmu xi gl xext x11)'],
                  'libraries': ['<!@(pkg-config --libs-only-l xmu xi gl xext x11)'],
                },
              },
            }],
            ['OS == "win"', {
              'direct_dependent_settings': {
                'link_settings': {
                  'libraries': ['-lglu32.lib', '-lopengl32.lib'],
                },
              },
            }],
          ],
        }, #end target "glew"
      ], #end targets
    }, {
      'targets': [
        {
          'target_name': 'glew',
          'type': 'none',
          'direct_dependent_settings': {
            'cflags': ['<!@(pkg-config --cflags glew)'],
            'link_settings': {
              'ldflags': ['<!@(pkg-config --libs-only-L --libs-only-other glew)'],
              'libraries': ['<!@(pkg-config --libs-only-l glew)'],
            },
          }, #end root.targets[glew].direct_dependent_settings
        }, #end target "glew"
      ], #end targets
    }],
  ],
} #end root
# vim: set expandtab tabstop=2 shiftwidth=2:
