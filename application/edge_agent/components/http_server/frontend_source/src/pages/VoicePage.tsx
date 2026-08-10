import { createEffect, createSignal, type Component } from 'solid-js';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { CollapsibleConfigBlock, StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Button } from '../components/ui/Button';
import { pushToast } from '../state/toast';

export const VoicePage: Component = () => {
  const { form, setForm, original, isDirty, isSaving, save, reset } = createConfigTab('voice', [
    'asr_provider',
    'asr_api_key',
    'asr_app_id',
    'asr_resource_id',
    'asr_base_url',
    'tts_provider',
    'tts_api_key',
    'tts_app_id',
    'tts_voice_type',
    'tts_base_url',
  ]);

  const applyVolcengineDefaults = () => {
    setForm('asr_provider', 'volcengine_sensevoice');
    setForm('asr_resource_id', 'volc.sensevoice.v1');
    setForm('asr_base_url', 'https://openspeech.bytedance.com/api/v1/asr');
    setForm('tts_provider', 'volcengine_tts');
    setForm('tts_voice_type', 'BV001_streaming');
    setForm('tts_base_url', 'https://openspeech.bytedance.com/api/v1/tts');
    pushToast('已填充火山 Engine SenseVoice ASR 与 TTS 默认配置项', 'success');
  };

  return (
    <TabShell tabId="voice">
      <div class="space-y-6 max-w-4xl">
        <PageHeader
          title="语音服务设置 (ASR & TTS)"
          subtitle="配置语音转文字 (ASR) 与文字转语音 (TTS) 服务，集成火山 Engine SenseVoice 实现端侧语音交互。"
        />

        <div class="flex items-center space-x-3 bg-neutral-900/50 p-4 rounded-xl border border-neutral-800">
          <Button variant="secondary" onClick={applyVolcengineDefaults}>
            填充火山引擎 SenseVoice 默认预设
          </Button>
        </div>

        {/* ASR Config Block */}
        <StaticConfigBlock
          title="语音识别 (ASR) 服务配置"
          description="配置用于将设备麦克风录入音频转换为文本的大模型 ASR 服务。"
        >
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <TextInput
              label="ASR 提供商 (Provider)"
              value={form.asr_provider || ''}
              onInput={(v) => setForm('asr_provider', v)}
              placeholder="volcengine_sensevoice"
            />
            <TextInput
              label="ASR Resource ID (资源标识)"
              value={form.asr_resource_id || ''}
              onInput={(v) => setForm('asr_resource_id', v)}
              placeholder="volc.sensevoice.v1"
            />
            <TextInput
              label="火山 App ID"
              value={form.asr_app_id || ''}
              onInput={(v) => setForm('asr_app_id', v)}
              placeholder="输入火山引擎 App ID"
            />
            <TextInput
              label="ASR API Key / Token"
              type="password"
              value={form.asr_api_key || ''}
              onInput={(v) => setForm('asr_api_key', v)}
              placeholder="输入火山引擎 Access Token"
            />
            <div class="md:col-span-2">
              <TextInput
                label="ASR 服务 Base URL"
                value={form.asr_base_url || ''}
                onInput={(v) => setForm('asr_base_url', v)}
                placeholder="https://openspeech.bytedance.com/api/v1/asr"
              />
            </div>
          </div>
        </StaticConfigBlock>

        {/* TTS Config Block */}
        <StaticConfigBlock
          title="语音合成 (TTS) 服务配置"
          description="配置用于将 Agent 回答转换为高品质语音播放的 TTS 服务。"
        >
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <TextInput
              label="TTS 提供商 (Provider)"
              value={form.tts_provider || ''}
              onInput={(v) => setForm('tts_provider', v)}
              placeholder="volcengine_tts"
            />
            <TextInput
              label="音色标识 (Voice Type)"
              value={form.tts_voice_type || ''}
              onInput={(v) => setForm('tts_voice_type', v)}
              placeholder="BV001_streaming"
            />
            <TextInput
              label="火山 App ID"
              value={form.tts_app_id || ''}
              onInput={(v) => setForm('tts_app_id', v)}
              placeholder="输入火山引擎 App ID"
            />
            <TextInput
              label="TTS API Key / Token"
              type="password"
              value={form.tts_api_key || ''}
              onInput={(v) => setForm('tts_api_key', v)}
              placeholder="输入火山引擎 Access Token"
            />
            <div class="md:col-span-2">
              <TextInput
                label="TTS 服务 Base URL"
                value={form.tts_base_url || ''}
                onInput={(v) => setForm('tts_base_url', v)}
                placeholder="https://openspeech.bytedance.com/api/v1/tts"
              />
            </div>
          </div>
        </StaticConfigBlock>

        <SavePanel isDirty={isDirty()} isSaving={isSaving()} onSave={save} onReset={reset} />
      </div>
    </TabShell>
  );
};
