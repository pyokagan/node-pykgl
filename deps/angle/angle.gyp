#use_system_angle does not exist yet
{
  'targets': [
    {
      'target_name': 'angle_translator_glsl',
      'type': 'none',
      'dependencies': ['angleproject/src/build_angle.gyp:translator_glsl'],
      'direct_dependent_settings': {
        'include_dirs': ['angleproject/include'],
      },
    },
  ],
}
# vim: set expandtab tabstop=2 shiftwidth=2:
