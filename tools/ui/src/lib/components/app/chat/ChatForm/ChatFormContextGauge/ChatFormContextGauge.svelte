<script lang="ts">
	import ContextGaugeDial from './ContextGaugeDial.svelte';
	import {
		gaugeTriggerClick,
		gaugeTriggerEnter,
		gaugeTriggerKeydown,
		gaugeTriggerLeave,
		gaugeTriggerPointerDown
	} from './gauge-popup.svelte';
	import { useContextGauge } from '$lib/hooks/use-context-gauge.svelte';
	import { colorLevelBgClass, colorLevelTextClass } from './context-gauge';
	import { formatParameters } from '$lib/utils/formatters';
	import { chatStore, conversationsStore } from '$lib/stores';
	import { untrack } from 'svelte';

	const gauge = useContextGauge();

	$effect(() => {
		const conv = conversationsStore.activeConversation;

		untrack(() => chatStore.processing.setActiveConversation(conv?.id ?? null));
	});

	$effect(() => {
		const conv = conversationsStore.activeConversation;
		const messages = conversationsStore.activeMessages as DatabaseMessage[];

		if (!conv) return;

		if (chatStore.isLoading || chatStore.isStreaming()) return;

		if (messages.length === 0) {
			untrack(() => chatStore.processing.setState(conv.id, null));

			return;
		}

		untrack(() => chatStore.processing.restoreFromMessages(messages, conv.id));
	});

	$effect(() => {
		gauge.startMonitoring();
	});

	const displayPercent = $derived(gauge.contextPercent !== null ? gauge.contextPercent : 0);
	const usedTokensStr = $derived(formatParameters(gauge.contextUsed));
	const totalTokensStr = $derived(gauge.contextTotal !== null ? formatParameters(gauge.contextTotal) : '65.5K');
</script>

<div
	role="button"
	tabindex="0"
	aria-label="Context usage bar"
	data-context-gauge-trigger
	class="flex items-center gap-2 px-2.5 py-1 rounded-full bg-muted/40 hover:bg-muted/70 transition-all border border-border/50 cursor-pointer select-none text-xs shadow-xs"
	onclick={gaugeTriggerClick}
	onkeydown={gaugeTriggerKeydown}
	onpointerdown={gaugeTriggerPointerDown}
	onpointerenter={gaugeTriggerEnter}
	onpointerleave={gaugeTriggerLeave}
	title="Context: {gauge.contextUsed.toLocaleString()} / {gauge.contextTotal?.toLocaleString() ?? '65,536'} tokens ({displayPercent}%)"
>
	<ContextGaugeDial percent={gauge.contextPercent} level={gauge.colorLevel} />

	<div class="flex items-center gap-1.5 font-mono text-[11px]">
		<div class="w-12 sm:w-16 md:w-24 h-1.5 rounded-full bg-muted-foreground/20 overflow-hidden relative">
			<div
				class="h-full rounded-full transition-all duration-300 {colorLevelBgClass(gauge.colorLevel)}"
				style="width: {Math.min(100, Math.max(displayPercent, gauge.contextUsed > 0 ? 2 : 0))}%"
			></div>
		</div>

		<span class="font-medium {colorLevelTextClass(gauge.colorLevel)}">
			{usedTokensStr}/{totalTokensStr}
		</span>
		<span class="text-muted-foreground/70 hidden sm:inline">
			({displayPercent}%)
		</span>
	</div>
</div>
