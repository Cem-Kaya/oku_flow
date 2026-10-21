"""Receipt/visualization invariants; standard library, no models or GPU."""
import copy
import unittest

from precision_report import compare, edits, geometry
from precision_overlay import annotations, page


def word(text,index=0,disposition=0):
    return {"text":text,"disposition":disposition,"confidence":.9,
            "bounds":[index*20,0,18,10],"protectedBounds":[index*20,0,18,10]}


def record(tokens):
    return {"result":{"sourcePixelDigest":"same","deviceInput":True,"output":{"ok":True,"lines":[tokens]}},
            "meters":{"imageEgressCalls":0},"profile":{"orderedCropDescriptorSha256":"crop",
                "detectorPasses":0,"batchCapacity":8,"recognizerWidthPolicy":"fixed1024"}}


class ReceiptTests(unittest.TestCase):
    def test_edit_script_keeps_insertions_drops_and_substitutions(self):
        distance,script=edits(['a','b','c'],['a','x','c','d'])
        self.assertEqual(distance,2)
        self.assertEqual([s['kind'] for s in script],['equal','changed','equal','added'])
        self.assertEqual(edits(['a','b'],['a'])[1][-1]['kind'],'dropped')

    def test_exact_five_percent_boundary(self):
        for count,expected in [(20,True),(19,False)]:
            a=record([word(str(i),i) for i in range(count)]);b=copy.deepcopy(a)
            b['result']['output']['lines'][0][0]['text']='changed'
            self.assertEqual(compare(a,b,False)['withinOwnerFivePercent'],expected)

    def test_image_egress_refused(self):
        a=record([word('a')]);b=copy.deepcopy(a);b['meters']['imageEgressCalls']=1
        with self.assertRaisesRegex(ValueError,'egress'):compare(a,b,False)

    def test_source_mismatch_refused(self):
        a=record([word('a')]);b=copy.deepcopy(a);b['result']['sourcePixelDigest']='other'
        with self.assertRaisesRegex(ValueError,'fingerprints'):compare(a,b,False)

    def test_frozen_crop_mismatch_refused(self):
        a=record([word('a')]);b=copy.deepcopy(a);b['profile']['orderedCropDescriptorSha256']='other'
        with self.assertRaisesRegex(ValueError,'Frozen'):compare(a,b,True)

    def test_timed_detector_in_frozen_refused(self):
        a=record([word('a')]);b=copy.deepcopy(a);b['profile']['detectorPasses']=1
        with self.assertRaisesRegex(ValueError,'Frozen'):compare(a,b,True)

    def test_batch_or_width_change_refused(self):
        for key,value in [('batchCapacity',1),('recognizerWidthPolicy','bucketed')]:
            a=record([word('a')]);b=copy.deepcopy(a);b['profile'][key]=value
            with self.assertRaisesRegex(ValueError,'Batch/width'):compare(a,b,False)

    def test_spatial_occurrences_do_not_swap_repeated_words(self):
        a=[word('the',0),word('the',10)]
        b=[word('the',10),word('the',0)]
        result=geometry(a,b)
        self.assertEqual(result['maxCommonWordEdgePixels'],0)
        self.assertEqual(result['orderDescents'],1)

    def test_confidence_and_disposition_changes_remain_visible(self):
        a=record([word('a')]);b=copy.deepcopy(a)
        b['result']['output']['lines'][0][0].update(confidence=.6,disposition=1)
        result=compare(a,b,False)
        self.assertEqual(result['candidateEdits'],1)
        self.assertAlmostEqual(result['geometry']['pairs'][0]['confidenceDelta'],-.3)
        self.assertEqual(result['geometry']['pairs'][0]['dispositionAfter'],1)

    def test_overlay_omitted_word_is_red_category(self):
        a=record([word('a',0),word('b',10)]);b=record([word('a')])
        result=annotations(a,b)
        self.assertEqual(result[0]['kind'],'omitted')
        self.assertEqual(result[0]['before'],'b')

    def test_overlay_suppression_is_distinguished(self):
        a=record([word('a')]);b=record([word('a',disposition=1)])
        result=annotations(a,b)
        self.assertEqual(result[0]['kind'],'suppressed')

    def test_overlay_does_not_call_reordered_existing_word_missing(self):
        a=record([word('a',0),word('b',10)]);b=record([word('b',10),word('a',0)])
        self.assertEqual(annotations(a,b),[])

    def test_overlay_changed_reading_is_separate(self):
        result=annotations(record([word('a')]),record([word('b')]))
        self.assertEqual(result[0]['kind'],'changed')
        self.assertEqual(result[0]['after'],'b')

    def test_ocr_text_cannot_close_html_script(self):
        value='</script><script>alert(1)</script>'
        item={'id':1,'kind':'changed','before':value,'after':'x','bounds':[0,0,10,10]}
        html=page('Z:/frame.png',100,100,[item],'baseline.json','candidate.json')
        self.assertNotIn(value,html)
        self.assertIn('\\u003c/script\\u003e',html)


if __name__=='__main__':unittest.main()
