uefi_vars_ss = ss.source_set()
uefi_vars_ss.add(when: 'CONFIG_UEFI_VARS',
                 if_true: files('var-service-core.c',
                                'var-service-json.c',
                                'var-service-vars.c',
                                'var-service-auth.c',
                                'var-service-guid.c',
                                'var-service-utils.c',
                                'var-service-policy.c',
                                'var-service-siglist.c'))

if (config_all_devices.has_key('CONFIG_UEFI_VARS'))
  uefi_vars_ss.add(when: gnutls,
                   if_true: files('var-service-pkcs7.c'),
                   if_false: files('var-service-pkcs7-stub.c'))
endif

modules += { 'hw-uefi' : {
    'vars'     : uefi_vars_ss,
}}
