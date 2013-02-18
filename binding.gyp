{
  'targets': [
    {
      'target_name': 'pykgl',
      'sources': ['src/pykgl.cpp', 'src/glconstants.cpp'],
      'dependencies': [
        'deps/angle/angle.gyp:angle_translator_glsl',
        'deps/glew/glew.gyp:glew',
      ]
    },
  ],
}
# vim: set expandtab tabstop=2 shiftwidth=2:
