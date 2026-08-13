import type { Component } from 'solid-js';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Button } from '../components/ui/Button';
import { pushToast } from '../state/toast';

type VoiceForm = {
  voice_api_key: string;
  asr_resource_id: string;
  asr_base_url: string;
  tts_resource_id: string;
  tts_speaker: string;
  tts_base_url: string;
  voice_volume: string;
};

/** Volcengine (Doubao) endpoints this firmware speaks. */
const VOLCENGINE_DEFAULTS = {
  asr_resource_id: 'volc.seedasr.sauc.duration',
  asr_base_url: 'wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream',
  tts_resource_id: 'seed-tts-2.0',
  tts_speaker: 'zh_female_vv_uranus_bigtts',
  tts_base_url: 'https://openspeech.bytedance.com/api/v3/tts/unidirectional',
} as const;

/** Software attenuation only; see cap_voice_pipeline's voice_audio_io. */
const clampVolume = (value: string): number => {
  const parsed = Number.parseInt(value, 10);

  if (Number.isNaN(parsed)) {
    return 100;
  }
  return Math.min(100, Math.max(0, parsed));
};

export const VoicePage: Component = () => {
  const tab = createConfigTab<VoiceForm>({
    tab: 'voice',
    groups: ['voice'],
    toForm: (config: Partial<AppConfig>) => ({
      voice_api_key: config.voice_api_key ?? '',
      asr_resource_id: config.asr_resource_id ?? '',
      asr_base_url: config.asr_base_url ?? '',
      tts_resource_id: config.tts_resource_id ?? '',
      tts_speaker: config.tts_speaker ?? '',
      tts_base_url: config.tts_base_url ?? '',
      voice_volume: config.voice_volume ?? '100',
    }),
    fromForm: (form) => ({
      voice_api_key: form.voice_api_key.trim(),
      asr_resource_id: form.asr_resource_id.trim(),
      asr_base_url: form.asr_base_url.trim(),
      tts_resource_id: form.tts_resource_id.trim(),
      tts_speaker: form.tts_speaker.trim(),
      tts_base_url: form.tts_base_url.trim(),
      /* Clamp here rather than rejecting the save: the firmware ignores an
       * out-of-range value and keeps the old volume, which would look like the
       * save silently did nothing. */
      voice_volume: String(clampVolume(form.voice_volume)),
    }),
  });

  const applyVolcengineDefaults = () => {
    tab.setForm({ ...VOLCENGINE_DEFAULTS });
    pushToast('已填充火山引擎默认端点，仍需填写 API Key', 'success');
  };

  return (
    <TabShell>
      <PageHeader
        title="语音服务 (ASR / TTS)"
        description="接入火山引擎豆包语音：流式语音识别把你说的话送进 Agent，流式语音合成把回答播出来。"
        actions={
          <Button variant="secondary" size="sm" onClick={applyVolcengineDefaults}>
            填充默认端点
          </Button>
        }
      />

      <StaticConfigBlock title="鉴权">
        <div class="grid gap-3">
          <TextInput
            full
            type="password"
            label="语音服务 API Key"
            placeholder="火山引擎控制台 > 语音技术 > API Keys"
            hint="新版控制台只需这一个 Key，识别与合成共用。旧版控制台的 App ID / Access Token 已不再需要。"
            value={tab.form.voice_api_key}
            onInput={(event) => tab.setForm('voice_api_key', event.currentTarget.value)}
          />
        </div>
      </StaticConfigBlock>

      <StaticConfigBlock title="语音识别 (ASR)">
        <div class="grid gap-3 sm:grid-cols-2">
          <TextInput
            label="Resource ID"
            placeholder="volc.seedasr.sauc.duration"
            hint="豆包 2.0 小时版。1.0 为 volc.bigasr.sauc.duration，并发版把 duration 换成 concurrent。"
            value={tab.form.asr_resource_id}
            onInput={(event) => tab.setForm('asr_resource_id', event.currentTarget.value)}
          />
          <TextInput
            label="服务地址"
            placeholder="wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream"
            hint="流式输入模式，须为 wss:// 地址。"
            value={tab.form.asr_base_url}
            onInput={(event) => tab.setForm('asr_base_url', event.currentTarget.value)}
          />
        </div>
      </StaticConfigBlock>

      <StaticConfigBlock title="语音合成 (TTS)">
        <div class="grid gap-3 sm:grid-cols-2">
          <TextInput
            label="Resource ID"
            placeholder="seed-tts-2.0"
            hint="复刻音色请填 seed-icl-2.0。"
            value={tab.form.tts_resource_id}
            onInput={(event) => tab.setForm('tts_resource_id', event.currentTarget.value)}
          />
          <TextInput
            label="音色 (speaker)"
            placeholder="zh_female_vv_uranus_bigtts"
            hint="音色列表见火山控制台。"
            value={tab.form.tts_speaker}
            onInput={(event) => tab.setForm('tts_speaker', event.currentTarget.value)}
          />
          <TextInput
            full
            label="服务地址"
            placeholder="https://openspeech.bytedance.com/api/v3/tts/unidirectional"
            hint="单向流式合成，音频边合成边播放。"
            value={tab.form.tts_base_url}
            onInput={(event) => tab.setForm('tts_base_url', event.currentTarget.value)}
          />
        </div>
      </StaticConfigBlock>

      <StaticConfigBlock title="播放">
        <div class="grid gap-3 sm:grid-cols-2">
          <TextInput
            type="number"
            label="音量 (0-100)"
            placeholder="100"
            hint="本机功放（MAX98357A 一类）没有增益接口，音量由固件在送进 I2S 前对 PCM 做衰减实现。50 约为 -12 dB。请勿用 audio Lua 模块调音量：那会关闭编解码设备并禁用 I2S 通道。"
            value={tab.form.voice_volume}
            onInput={(event) => tab.setForm('voice_volume', event.currentTarget.value)}
          />
        </div>
      </StaticConfigBlock>

      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => void tab.save()}
        onDiscard={tab.discard}
        message={tab.error()}
      />
    </TabShell>
  );
};
