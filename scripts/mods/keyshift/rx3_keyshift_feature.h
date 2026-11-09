/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_KEYSHIFT_FEATURE_H
#define RX3_KEYSHIFT_FEATURE_H
static int keyshift_feature_configured(void)
{
    const char *value=getenv("RX3_KEYSHIFT");
    return value && value[0]=='1';
}
/* SHIFT + BEAT SYNC: bring this deck to the MASTER's key. BEAT SYNC alone
   stays native. The shortcut always seeks the same key (identical mode, the
   smallest shift, never past +/-12), whatever the KEY tab's sync mode says.
   Press and release are both taken while SHIFT is held, so neither the
   native beat sync nor any native SHIFT+SYNC function sees half a gesture.
   The channel byte names the deck as 1 or 2; anything else passes through. */
#define KEYSHIFT_KEY_SYNC 0x4112u
static unsigned int keyshift_shift_sync_enabled;
static volatile unsigned int keyshift_sync_captured;
static unsigned int keyshift_shift_sync_count;

static int keyshift_master_delta(unsigned int deck)
{
    struct rx3_harmonic_reference ref=keyshift_reference(deck);
    if(deck>1u || ref.deck==(int)deck)return 0;
    return rx3_harmonic_delta(keyshift_base_key(deck),rx3_keyshift_semitones(deck),
        ref.key,ref.native_keys,0u,12u,0);
}

static int keyshift_shift_sync(const struct rx3_key_event *event)
{
    if(!keyshift_shift_sync_enabled || event->key!=KEYSHIFT_KEY_SYNC ||
       event->channel<1u || event->channel>2u) return 0;
    unsigned int bit=1u<<event->channel;
    if(event->operation==0u) {
        if(!framework->input->shift_held(event->channel)) return 0;
        unsigned int deck=event->channel-1u;
        int delta=keyshift_master_delta(deck);
        if(delta) {
            rx3_keyshift_change(deck,delta);
            if(framework->browse && framework->browse->deck_key)
                framework->browse->deck_key(deck,keyshift_base_key(deck),rx3_keyshift_semitones(deck));
            __atomic_store_n(&keyshift_refresh_pending,1u,__ATOMIC_SEQ_CST);
            __atomic_add_fetch(&keyshift_shift_sync_count,1u,__ATOMIC_SEQ_CST);
        }
        __atomic_fetch_or(&keyshift_sync_captured,bit,__ATOMIC_SEQ_CST);
        return 1;
    }
    if(__atomic_load_n(&keyshift_sync_captured,__ATOMIC_SEQ_CST)&bit) {
        if(event->operation==2u || event->operation==3u)
            __atomic_fetch_and(&keyshift_sync_captured,~bit,__ATOMIC_SEQ_CST);
        return 1;
    }
    return 0;
}

/* RX3_KEY_SYNC_SHIFT=0 turns the shortcut off; absent means on. */
static int keyshift_parse_shift_sync(const char *value)
{
    return !(value && value[0]=='0' && value[1]==0);
}

static int keyshift_equals(const char *a, const char *b)
{
    while(*a && *a==*b){a++;b++;}
    return *a==*b;
}
/* RX3_KEYSHIFT_ENGINE: wsola (default), stft or stft-both. Measured on the
   RX3 the STFT costs about 30% of each 64-frame block per deck and overran
   the block budget, so it is opt-in until it is made cheaper (DA_FIXARE 44). */
static unsigned int keyshift_parse_engine(const char *value)
{
    if(value && keyshift_equals(value,"stft"))return KEYSHIFT_ENGINE_STFT;
    if(value && keyshift_equals(value,"stft-both"))return KEYSHIFT_ENGINE_STFT_BOTH;
    return KEYSHIFT_ENGINE_WSOLA;
}
/* RX3_KEYSHIFT_BLOCK: 5120 (default), 4096 or 2048 frames. */
static unsigned int keyshift_parse_block(const char *value)
{
    if(value && keyshift_equals(value,"4096"))return 4096u;
    if(value && keyshift_equals(value,"2048"))return 2048u;
    return 5120u;
}

static int keyshift_feature_install(const struct rx3_services *services)
{
    framework=services;
    keyshift_engine=keyshift_parse_engine(getenv("RX3_KEYSHIFT_ENGINE"));
    keyshift_block=keyshift_parse_block(getenv("RX3_KEYSHIFT_BLOCK"));
    const char *sync=getenv("RX3_KEY_SYNC");
    keyshift_sync_enabled=sync && sync[0]=='1' && sync[1]==0;
    keyshift_sync_range=keyshift_parse_sync_range(getenv("RX3_KEY_SYNC_RANGE"));
    keyshift_sync_harmonic=keyshift_parse_sync_mode(getenv("RX3_KEY_SYNC_MODE"));
    keyshift_match_rules=rx3_match_rules(getenv("RX3_KEY_MATCH_RULES"));
    rx3_keyshift_install();
    if(rx3_keyshift_ready())keyshift_metadata_install();
    if(!rx3_keyshift_ready() || !framework->panels->register_row(&keyshift_row))return 0;
    /* The shortcut is optional: without the input service the KEY tab works. */
    keyshift_shift_sync_enabled=0;
    if(keyshift_parse_shift_sync(getenv("RX3_KEY_SYNC_SHIFT")) && framework->input &&
       framework->input->register_key &&
       framework->input->register_key(&keyshift_row,15u,keyshift_shift_sync))
        keyshift_shift_sync_enabled=1;
    return 1;
}
static void keyshift_feature_remove(void)
{
    if(keyshift_shift_sync_enabled && framework->input && framework->input->unregister_owner)
        framework->input->unregister_owner(&keyshift_row);
    keyshift_shift_sync_enabled=0;
    __atomic_store_n(&keyshift_sync_captured,0u,__ATOMIC_SEQ_CST);
    keyshift_metadata_remove();
    framework->panels->unregister_row(&keyshift_row);
    if(framework->browse && framework->browse->deck_key) {
        framework->browse->deck_key(0,-1,0);framework->browse->deck_key(1,-1,0);
    }
    rx3_keyshift_remove();
}
static void keyshift_feature_track_will_load(unsigned int deck, void *reader,
                                           const void *track_info)
{
    (void)reader; (void)track_info;
    /* Native metadata can precede PcmReader::load. The native cache
       writers own invalidation when the observer is active. */
    if(keyshift_metadata_active)return;
    if(deck<2)__atomic_store_n(&keyshift_track_key[deck],-1,__ATOMIC_SEQ_CST);
    if(framework->browse && framework->browse->deck_key)framework->browse->deck_key(deck,-1,0);
    __atomic_store_n(&keyshift_refresh_pending,1u,__ATOMIC_SEQ_CST);
}
static void keyshift_feature_track_did_load(unsigned int deck, void *reader,
                                           const void *track_info)
{
    (void)reader; (void)track_info;
    rx3_keyshift_reload(deck);
    if(deck<2 && framework->browse && framework->browse->deck_key)
        framework->browse->deck_key(deck,keyshift_base_key(deck),0);
    __atomic_store_n(&keyshift_refresh_pending,1u,__ATOMIC_SEQ_CST);
}
#endif
