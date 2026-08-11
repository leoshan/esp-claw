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
  asr_provider: string;
  asr_api_key: string;
  asr_app_id: string;
  asr_resource_id: string;
  asr_base_url: string;
  tts_provider: string;
  tts_api_key: string;
  tts_app_id: string;
  tts_voice_type: string;
  tts_base_url: string;
};

const VOLCENGINE_DEFAULTS = {
  asr_provider: 'volcengine_sensevoice',
  asr_resource_id: 'volc.sensevoice.v1',
  asr_base_url: 'https://openspeech.bytedance.com/api/v1/asr',
  tts_provider: 'volcengine_tts',
  tts_voice_type: 'BV001_streaming',
  tts_base_url: 'https://openspeech.bytedance.com/api/v1/tts',
} as const;

export const VoicePage: Component = () => {
  const tab = createConfigTab<VoiceForm>({
    tab: 'voice',
    groups: ['voice'],
    toForm: (config: Partial<AppConfig>) => ({
      asr_provider: config.asr_provider ?? '',
      asr_api_key: config.asr_api_key ?? '',
      asr_app_id: config.asr_app_id ?? '',
      asr_resource_id: config.asr_resource_id ?? '',
      asr_base_url: config.asr_base_url ?? '',
      tts_provider: config.tts_provider ?? '',
      tts_api_key: config.tts_api_key ?? '',
      tts_app_id: config.tts_app_id ?? '',
      tts_voice_type: config.tts_voice_type ?? '',
      tts_base_url: config.tts_base_url ?? '',
    }),
    fromForm: (form) => ({
      asr_provider: form.asr_provider.trim(),
      asr_api_key: form.asr_api_key.trim(),
      asr_app_id: form.asr_app_id.trim(),
      asr_resource_id: form.asr_resource_id.trim(),
      asr_base_url: form.asr_base_url.trim(),
      tts_provider: form.tts_provider.trim(),
      tts_api_key: form.tts_api_key.trim(),
      tts_app_id: form.tts_app_id.trim(),
      tts_voice_type: form.tts_voice_type.trim(),
      tts_base_url: form.tts_base_url.trim(),
    }),
  });

  const applyVolcengineDefaults = () => {
    tab.setForm({ ...VOLCENGINE_DEFAULTS });
    pushToast('已填充火山引擎 SenseVoice ASR 与 TTS 默认配置项', 'success');
  };

  return (
    <TabShell>
      <PageHeader
        title="语音服务设置 (ASR & TTS)"
        description="配置语音转文字 (ASR) 与文字转语音 (TTS) 服务，集成火山引擎 SenseVoice 实现端侧语音交互。"
        actions={
          <Button variant="secondary" size="sm" onClick={applyVolcengineDefaults}>
            填充火山引擎默认预设
          </Button>
        }
      />

      <StaticConfigBlock title="语音识别 (ASR) 服务配置">
        <div class="grid gap-3 sm:grid-cols-2">
          <TextInput
            label="ASR 提供商 (Provider)"
            placeholder="volcengine_sensevoice"
            value={tab.form.asr_provider}
            onInput={(event) => tab.setForm('asr_provider', event.currentTarget.value)}
          />
          <TextInput
            label="ASR Resource ID (资源标识)"
            placeholder="volc.sensevoice.v1"
            value={tab.form.asr_resource_id}
            onInput={(event) => tab.setForm('asr_resource_id', event.currentTarget.value)}
          />
          <TextInput
            label="火山引擎 App ID"
            placeholder="输入火山引擎 App ID"
            value={tab.form.asr_app_id}
            onInput={(event) => tab.setForm('asr_app_id', event.currentTarget.value)}
          />
          <TextInput
            type="password"
            label="ASR API Key / Token"
            placeholder="输入火山引擎 Access Token"
            value={tab.form.asr_api_key}
            onInput={(event) => tab.setForm('asr_api_key', event.currentTarget.value)}
          />
          <TextInput
            full
            label="ASR 服务 Base URL"
            placeholder="https://openspeech.bytedance.com/api/v1/asr"
            value={tab.form.asr_base_url}
            onInput={(event) => tab.setForm('asr_base_url', event.currentTarget.value)}
          />
        </div>
      </StaticConfigBlock>

      <StaticConfigBlock title="语音合成 (TTS) 服务配置">
        <div class="grid gap-3 sm:grid-cols-2">
          <TextInput
            label="TTS 提供商 (Provider)"
            placeholder="volcengine_tts"
            value={tab.form.tts_provider}
            onInput={(event) => tab.setForm('tts_provider', event.currentTarget.value)}
          />
          <TextInput
            label="音色标识 (Voice Type)"
            placeholder="BV001_streaming"
            value={tab.form.tts_voice_type}
            onInput={(event) => tab.setForm('tts_voice_type', event.currentTarget.value)}
          />
          <TextInput
            label="火山引擎 App ID"
            placeholder="输入火山引擎 App ID"
            value={tab.form.tts_app_id}
            onInput={(event) => tab.setForm('tts_app_id', event.currentTarget.value)}
          />
          <TextInput
            type="password"
            label="TTS API Key / Token"
            placeholder="输入火山引擎 Access Token"
            value={tab.form.tts_api_key}
            onInput={(event) => tab.setForm('tts_api_key', event.currentTarget.value)}
          />
          <TextInput
            full
            label="TTS 服务 Base URL"
            placeholder="https://openspeech.bytedance.com/api/v1/tts"
            value={tab.form.tts_base_url}
            onInput={(event) => tab.setForm('tts_base_url', event.currentTarget.value)}
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
