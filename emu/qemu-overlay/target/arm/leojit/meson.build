leo_decodetree = generator(find_program(meson.project_source_root() / 'scripts/decodetree.py'),
                           output: 'leo-decode-@BASENAME@.c.inc',
                           arguments: ['@INPUT@', '@EXTRA_ARGS@', '-o', '@OUTPUT@'])

leo_tcg = meson.project_source_root() / 'target/arm/tcg'
arm_ss.add(when: 'TARGET_AARCH64', if_false: [
  leo_decodetree.process(leo_tcg / 'a32.decode',
                         extra_args: ['--static-decode=leo_disas_a32', '--translate=leo']),
  leo_decodetree.process(leo_tcg / 'a32-uncond.decode',
                         extra_args: ['--static-decode=leo_disas_a32_uncond', '--translate=leo']),
  leo_decodetree.process(leo_tcg / 't32.decode',
                         extra_args: ['--static-decode=leo_disas_t32', '--translate=leo']),
  leo_decodetree.process(leo_tcg / 't16.decode',
                         extra_args: ['-w', '16', '--static-decode=leo_disas_t16', '--translate=leo']),
  files('frontend.c', 'interp.c', 'engine.c', 'backend.c'),
])
